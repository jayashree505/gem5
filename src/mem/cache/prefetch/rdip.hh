#ifndef __MEM_CACHE_PREFETCH_RDIP_HH__
#define __MEM_CACHE_PREFETCH_RDIP_HH__

#include <deque>
#include <vector>

#include "base/circular_queue.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"
#include "cpu/pred/rdip_ras.hh"
#include "cpu/base.hh"
#include "cpu/pred/btb.hh"
#include "arch/types.hh"
#include "base/logging.hh"
#include "base/types.hh"
#include "config/the_isa.hh"

/*Direction of Branches*/
#define BRANCH_CALL 1
#define BRANCH_RETURN 2
/*RAS Relates Macros */
#define RAS_SIZE 4
/*Signature related Macros*/
#define PREFETCH_LOOKAHEAD 1
#define PREVSIG_QUEUE_SIZE (1+PREFETCH_LOOKAHEAD)
/*Miss Table related Macros*/
#define INTRUCTION_MISS_COVERAGE 15
#define REGIONS_PER_SIGNATURE 3
/*Compactor related Macros*/
#define PREC_SPATIAL_REGION_BITS 3  /*Total 8 bit vector*/
#define SUCC_SPATIAL_REGION_BITS 5

extern bool RDIP_Call_Flag;  

extern bool RDIP_Return_Flag;  

extern TheISA::PCState RDIP_Branch_Target; 

struct RDIPPrefetcherParams;

namespace Prefetcher {

class RDIP : public Queued
{
    private:
        /** The per-thread return address stack. */
        RDIPReturnAddrStack RAS;
        
        /** Initialize RAS                       
         *  Identify if the instruction is a Call or Return
         *          If Call  : push RAS
         *          If Return: pop RAS
         */
        uint8_t RASHandler(const Addr pc);
        
        /** Current Program Context Signature: Updates on a Call or Return*/
        uint64_t CurSig = 0;  
        /** Previous Program Context Signature*/
        uint64_t PrevSig = 0;
        /** Previous Signature with Lookahead */
        uint64_t PrevSigQueue[PREVSIG_QUEUE_SIZE];
        /** Previous Signature Queue Position*/
        int PrevSigQ_pos =0;
        /** FIFO queue containing the sandbox entries. */
        CircularQueue<Addr> CurSigMissesBuffer;
        /** Spatial and Temporal compactor*/
        void RDIPCompator(Addr cirBuffAddr);
        /** Number of preceding and subsequent spatial addresses to compact */
        const unsigned int precSize;
        const unsigned int succSize;
        /** Number of entries used for the temporal compactor */
        const unsigned int maxCompactorEntries;
        /*Compactor- Spatial*/
        struct CompactorEntry {
            Addr trigger;
            std::vector<bool> prec;
            std::vector<bool> succ;
            CompactorEntry() {}
            CompactorEntry(Addr, unsigned int, unsigned int);

            bool inSameSpatialRegion(Addr addr, unsigned int log_blk_size,
                                     bool update);

            Addr distanceFromTrigger(Addr addr,
                                     unsigned int log_blk_size) const;
        };
        CompactorEntry spatialCompactor;
        /** Temporal compactor is a queue of spatial regions*/
        std::deque<CompactorEntry> temporalCompactor;
        /** Miss Table Update function*/
        void UpdateMissTable();
        /** Miss Table entry*/
        struct MissTableEntry : public TaggedEntry{
            //Lookahead considered for previous signatures
            uint64_t  PreviousSignature;
            //For every signature entry, regions(based on spatial and temporal) associated
            CompactorEntry TriggerAddress[REGIONS_PER_SIGNATURE];
        };
        /** Miss Table is a cache like structure   
         * No of entries, Associativity, indexing and replacement
         * policy passed as parameters */
        AssociativeSet<MissTableEntry> missTable;
        /** Generate Prefetches for the Current signature*/
        void GeneratePrefetches(unsigned int log_blk_size,
                    std::vector<AddrPriority> &addresses);

    public:
        RDIP(const RDIPPrefetcherParams *p);
        ~RDIP() = default;

        void calculatePrefetch(const PrefetchInfo &pfi,
                               std::vector<AddrPriority> &addresses);

};

} // namespace Prefetcher

#endif // __MEM_CACHE_PREFETCH_RDIP_HH__

