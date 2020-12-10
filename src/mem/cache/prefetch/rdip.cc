#include "mem/cache/prefetch/rdip.hh"

#include <utility>

#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/RDIPPrefetcher.hh"

namespace Prefetcher {

RDIP::RDIP(const RDIPPrefetcherParams *p)
    : Queued(p),
    CurSigMissesBuffer(INTRUCTION_MISS_COVERAGE),
    precSize(PREC_SPATIAL_REGION_BITS),
    succSize(SUCC_SPATIAL_REGION_BITS),
    maxCompactorEntries(REGIONS_PER_SIGNATURE),
    missTable(p->misstable_assoc, p->misstable_entries, 
              p->misstable_indexing_policy,
              p->misstable_replacement_policy)
{ 
    //Initialize RAS
    RAS.init(RAS_SIZE);   
    //Initialize Signatures
    CurSig = 0;  
    PrevSig = 0;
    PrevSigQ_pos =0;
}

uint8_t
RDIP::RASHandler(const Addr pc)
{
    uint8_t branch_type =0;
    //TheISA::PCState branch_target;
    
    //Identify if Call or Return
    if(RDIP_Call_Flag == true)
    {
        branch_type = BRANCH_CALL;
        RAS.push(pc);
        DPRINTF(HWPrefetch, "Instruction %s was a call, adding "
                        "%s to the RAS index: %i, RAS Top Address: %s\n",
                        pc, pc, RAS.topIdx(), RAS.top());
        //Update the Previous Signature after pushing into RAS for Call
        PrevSig = CurSig;
        //Update Previous Signature into the FIFO Queue for lookahead
        PrevSigQueue[PrevSigQ_pos] = PrevSig;
        PrevSigQ_pos = (PrevSigQ_pos+1)%(PREVSIG_QUEUE_SIZE);
        //Update the Current Signature after pushing into RAS for Call
        for (int i=0; i<RAS_SIZE; i++)
        {
            CurSig = CurSig ^ RAS.addrStack[i];
        }
        //Direction 0 indicates Call for Current Signature
        CurSig = CurSig | 0;
        DPRINTF(HWPrefetch, "CALL: Current Signatue: %s ,Previous Signature: %s \n",CurSig, PrevSig);
    }
    else if(RDIP_Return_Flag == true)
    {
        branch_type = BRANCH_RETURN;
        //Update the Previous Signature before popping from RAS for Return
        PrevSig = CurSig;
        //Update Previous Signature into the FIFO Queue for lookahead
        PrevSigQueue[PrevSigQ_pos] = PrevSig;
        PrevSigQ_pos = (PrevSigQ_pos+1)%(PREVSIG_QUEUE_SIZE);
        //Update the Current Signature before popping from RAS for Return
        //Direction 1 indicates Return for Current Signature
        CurSig = CurSig | 1;
        DPRINTF(HWPrefetch, "RETURN: Current Signature: %s ,Previous Signature: %s \n",CurSig, PrevSig);
        RAS.pop();
        DPRINTF(HWPrefetch, "Instruction %s is a return, "
                    " RAS index: %i, RAS Top Address: %s,\n",
                     pc, RAS.topIdx(), RAS.top());
    }
    else
    {
        branch_type = 0;
    }
    //Get correct Branch Target Address
    //branch_target = RDIP_Branch_Target;

    DPRINTF(HWPrefetch, "Branch Type %i\n", branch_type);
    //DPRINTF(HWPrefetch, "Branch Target %#lx\n", branch_target);

    //Reset the variables
    RDIP_Call_Flag = false;
    RDIP_Return_Flag = false;

    return branch_type;
}

RDIP::CompactorEntry::CompactorEntry(Addr addr,
    unsigned int prec_size, unsigned int succ_size)
{
    trigger = addr;
    prec.resize(prec_size, false);
    succ.resize(succ_size, false);
}

Addr
RDIP::CompactorEntry::distanceFromTrigger(Addr target,
        unsigned int log_blk_size) const
{
    const Addr target_blk = target >> log_blk_size;
    const Addr trigger_blk = trigger >> log_blk_size;

    return target_blk > trigger_blk ?
              target_blk - trigger_blk : trigger_blk - target_blk;
}

bool
RDIP::CompactorEntry::inSameSpatialRegion(Addr cirBuffAddr,
        unsigned int log_blk_size, bool update)
{
    bool hit = false;
    Addr blk_distance = distanceFromTrigger(cirBuffAddr, log_blk_size);

    if (blk_distance != 0)
    {
        hit = (cirBuffAddr > trigger) ?
            (succ.size() >= blk_distance) : (prec.size() >= blk_distance);
        if (hit && update) {
            if (cirBuffAddr > trigger) {
                succ[blk_distance - 1] = true;
            } else if (cirBuffAddr < trigger) {
                prec[blk_distance - 1] = true;
            }
        }
    }
    else
    {
        //No Blk difference: Same Block - Do not update vector
        hit = true;
    }
    
    return hit;
}

void
RDIP::RDIPCompator(Addr cirBuffAddr)
{
    //First entry to compactor
    if (temporalCompactor.size() == 0)
    {
        spatialCompactor = CompactorEntry(cirBuffAddr, precSize, succSize);
        temporalCompactor.push_back(spatialCompactor);
        DPRINTF(HWPrefetch, "First entry to compactor, Address: %s\n", cirBuffAddr);
    }
    else
    {
        //Check spatial region hit
        if(spatialCompactor.inSameSpatialRegion(cirBuffAddr, lBlkSize, true))
        {
            DPRINTF(HWPrefetch, "Spatial Region Hit \n");
        }
        else
        {   
            bool is_in_temporal_compactor = false;

            // Check if the PC is in the temporal compactor
            for (auto it = temporalCompactor.begin();
                    it != temporalCompactor.end(); it++)
            {
                if (it->inSameSpatialRegion(cirBuffAddr, lBlkSize, false)) {
                    spatialCompactor = (*it);
                    temporalCompactor.erase(it);
                    is_in_temporal_compactor = true;
                    DPRINTF(HWPrefetch, "Found already in Temporal Region \n");
                    break;
                }
            }

            if (temporalCompactor.size() == maxCompactorEntries) {
                temporalCompactor.pop_front(); // Discard the LRU entry
                DPRINTF(HWPrefetch, "Temporal Region Max reached\n");
            }

            temporalCompactor.push_back(spatialCompactor);
            DPRINTF(HWPrefetch, "Added to Temporal Region \n");

            // If the compactor entry is neither the spatial or can't be
            // found in the temporal compactor, reset the spatial compactor
            // updating the trigger address and resetting the vector bits
            if (!is_in_temporal_compactor) {
                DPRINTF(HWPrefetch, "Trigger Blk Add: %s, Target Blk Addr: %s\n", 
                                                        spatialCompactor.trigger,cirBuffAddr);
                // Reset the spatial compactor fields with the new address
                spatialCompactor = CompactorEntry(cirBuffAddr, precSize, succSize);
                DPRINTF(HWPrefetch, "Spatial Compactor field updated \n");
            } 
        } 
    }
}

void
RDIP::UpdateMissTable()
{
    //Get the data from temporal queue
    /*
    CompactorEntry test;
    for (auto it = temporalCompactor.begin();
            it != temporalCompactor.end(); it++)
    {
        test = (*it);
        DPRINTF(HWPrefetch, "Test: Miss Table: Trigger Address: %s\n"
                                ,test.trigger);
    }
    */
    MissTableEntry *miss_entry;
    //Check the miss table with the previous signatures (i.e with lookahead)
    //Update lookahead no of prev signatures
    for (auto sign_it =0; sign_it<PREVSIG_QUEUE_SIZE; sign_it++)
    {
        DPRINTF(HWPrefetch, "Miss Table: Previous Signature %d: %s \n"
                                ,sign_it,PrevSigQueue[sign_it] );
        miss_entry = missTable.findEntry(PrevSigQueue[sign_it], false);
        if (miss_entry != nullptr) 
        {
            missTable.accessEntry(miss_entry); //Access to update the indexing
            DPRINTF(HWPrefetch, "Miss Table: Previous Signature already found in miss table \n ");
        } 
        else 
        {
            miss_entry = missTable.findVictim(PrevSigQueue[sign_it]);
            assert(miss_entry != nullptr);
            DPRINTF(HWPrefetch, "Miss Table: New Previous Signature entry to Miss Table \n ");
            int region_no = 0;
            for (auto it = temporalCompactor.begin();
            it != temporalCompactor.end(); it++)
            {
                //Take all the regions per signature into the miss entry
                miss_entry->TriggerAddress[region_no]= (*it);
                DPRINTF(HWPrefetch, "Miss Table: Trigger Address: %s, Region no: %d \n"
                                ,miss_entry->TriggerAddress[region_no].trigger,region_no);
                region_no++;
            }
            missTable.insertEntry(PrevSigQueue[sign_it], false,
                                miss_entry);
        }
    }
}

void
RDIP::GeneratePrefetches(unsigned int log_blk_size,
    std::vector<AddrPriority> &addresses)
{
    MissTableEntry *miss_entry_prefetch;
    int region_no = 0;

    // Lookup the miss table with the Current Signature
    miss_entry_prefetch = missTable.findEntry(CurSig, false);
    if (miss_entry_prefetch != nullptr) 
    {
        missTable.accessEntry(miss_entry_prefetch); //Access to update the indexing
        DPRINTF(HWPrefetch, "Generate Prefetch: Current Signature found in Miss Table \n ");
         
        for (region_no = 0;region_no <REGIONS_PER_SIGNATURE; region_no++ )
        {
            const Addr trigger_base = miss_entry_prefetch->TriggerAddress[region_no].trigger; 
            std::vector<bool> prec_vector = miss_entry_prefetch->TriggerAddress[region_no].prec;
            std::vector<bool> succ_vector = miss_entry_prefetch->TriggerAddress[region_no].succ;

            const Addr trigger_blk = trigger_base >> log_blk_size;
            DPRINTF(HWPrefetch, "Prefetch: Region %d Start: Base:%s \n ",region_no,trigger_base);

            //Prefetch Base Trigger Address
            addresses.push_back(AddrPriority(trigger_base, 0));
            DPRINTF(HWPrefetch, "Generated Prefetch: Base Trigger: Address:%s  \n "
                                  ,trigger_base);

            for (int i = prec_vector.size()-1; i >= 0; i--) {
                // Address from the preceding blocks to issue a prefetch
                if (prec_vector[i]) {
                    const Addr prec_addr = (trigger_blk - (i+1)) << log_blk_size;
                    addresses.push_back(AddrPriority(prec_addr, 0));
                    DPRINTF(HWPrefetch, "Generated Prefetch: Prec Vector: %d: Prec Address:%s \n "
                                            ,i, prec_addr);
                }
            }
            for (int i = 0; i < succ_vector.size(); i++) {
                // Address from the succeding blocks to issue a prefetch
                if (succ_vector[i]) {
                    const Addr succ_addr = (trigger_blk + (i+1)) << log_blk_size;
                    addresses.push_back(AddrPriority(succ_addr, 0));
                    DPRINTF(HWPrefetch, "Generated Prefetch: Succ Vector: %d: Succ Address:%s \n "
                                            ,i, succ_addr);
                }
            }
        }
    }
    else
    {
        DPRINTF(HWPrefetch, "Generate Prefetch: No Entry- No Prefetch \n ");
    }
}

void
RDIP::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses)
{
    const Addr pc = pfi.getPC();
    Addr RegionBaseAddr = pfi.getPC();
    Addr CircularBuffAddr =0;
    uint8_t BranchType =0;
 
    //DPRINTF(HWPrefetch, "Entered RDIP\n");
    BranchType = RASHandler(pc);

    //Record Misses into Circular Queue
    if (pfi.isCacheMiss()) 
    {
        CurSigMissesBuffer.push_back(RegionBaseAddr);
        //DPRINTF(HWPrefetch, "Instruction %s Cache Miss, Region Addr: %s\n",pc, RegionBaseAddr);
    }
    //Record Misses into Miss Table if signature changes and update to miss table
    if ((BranchType == BRANCH_CALL)||(BranchType == BRANCH_RETURN))
    {
        /*compact to a base address with vector format*/
        for(int i =0 ;i<INTRUCTION_MISS_COVERAGE; i++)
        {
            CircularBuffAddr = *((CurSigMissesBuffer.end())-i);
            DPRINTF(HWPrefetch, "IRegion Addr %d: %s: PC: %s\n",i, CircularBuffAddr,pc);
            RDIPCompator(CircularBuffAddr);
        }
        //Clear Circular Misses Buffer after updating to miss table
        CurSigMissesBuffer.flush();
        
        //Update miss table entry
        UpdateMissTable();

        //Generate Prefetches for the current signature
        GeneratePrefetches(lBlkSize, addresses);
    }
}


} // namespace Prefetcher

Prefetcher::RDIP*
RDIPPrefetcherParams::create()
{
    return new Prefetcher::RDIP(this);
}


