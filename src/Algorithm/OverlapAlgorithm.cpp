//-----------------------------------------------
// Copyright 2009 Wellcome Trust Sanger Institute
// Written by Jared Simpson (js18@sanger.ac.uk)
// Released under the GPL
//-----------------------------------------------
#include "OverlapAlgorithm.h"
#include "ASQG.h"
#include <tr1/unordered_set>
#include <math.h>

// Collect the complete set of overlaps in pOBOut
static const AlignFlags sufPreAF(false, false, false);
static const AlignFlags prePreAF(false, true, true);
static const AlignFlags sufSufAF(true, false, true);
static const AlignFlags preSufAF(true, true, false);

//#define TEMPDEBUG 1

// Perform the overlap
OverlapResult OverlapAlgorithm::overlapRead(const SeqRecord& read, OverlapBlockList* pOutList) const
{
    OverlapResult r;
    r = overlapReadInexact(read, pOutList);
    //r = overlapReadExact(read, pOutList);
    return r;
}

//
OverlapResult OverlapAlgorithm::overlapReadInexact(const SeqRecord& read, OverlapBlockList* pOBOut) const
{
    OverlapResult result;
    OverlapBlockList obWorkingList;
    std::string seq = read.seq.toString();

#ifdef TEMPDEBUG
    std::cout << "Overlapping read " << read.id << " suffix\n";
#endif

    // Match the suffix of seq to prefixes
    findOverlapBlocksInexact(seq, m_pBWT, m_pRevBWT, sufPreAF, &obWorkingList, pOBOut, result);
    findOverlapBlocksInexact(complement(seq), m_pRevBWT, m_pBWT, prePreAF, &obWorkingList, pOBOut, result);

    if(m_bIrreducible)
    {
        computeIrreducibleBlocks(m_pBWT, m_pRevBWT, &obWorkingList, pOBOut);
        obWorkingList.clear();
    }
    else
    {
        pOBOut->splice(pOBOut->end(), obWorkingList);
        assert(obWorkingList.empty());
    }

#ifdef TEMPDEBUG
    std::cout << "Overlapping read " << read.id << " prefix\n";
#endif

    // Match the prefix of seq to suffixes
    findOverlapBlocksInexact(reverseComplement(seq), m_pBWT, m_pRevBWT, sufSufAF, &obWorkingList, pOBOut, result);
    findOverlapBlocksInexact(reverse(seq), m_pRevBWT, m_pBWT, preSufAF, &obWorkingList, pOBOut, result);

    if(m_bIrreducible)
    {
        computeIrreducibleBlocks(m_pBWT, m_pRevBWT, &obWorkingList, pOBOut);
        obWorkingList.clear();
    }
    else
    {
        pOBOut->splice(pOBOut->end(), obWorkingList);
        assert(obWorkingList.empty());
    }

    return result;
}

// Construct the set of blocks describing irreducible overlaps with READ
// and write the blocks to pOBOut
OverlapResult OverlapAlgorithm::overlapReadExact(const SeqRecord& read, OverlapBlockList* pOBOut) const
{
    OverlapResult result;

    // The complete set of overlap blocks are collected in obWorkingList
    // The filtered set (containing only irreducible overlaps) are placed into pOBOut
    // by calculateIrreducibleHits
    OverlapBlockList obWorkingList;
    std::string seq = read.seq.toString();

    // Irreducible overlaps only
    WARN_ONCE("Irreducible-only assumptions: All reads are the same length")

    // Match the suffix of seq to prefixes
    findOverlapBlocksExact(seq, m_pBWT, m_pRevBWT, sufPreAF, &obWorkingList, pOBOut, result);
    findOverlapBlocksExact(complement(seq), m_pRevBWT, m_pBWT, prePreAF, &obWorkingList, pOBOut, result);

    //    
    if(m_bIrreducible)
    {
        computeIrreducibleBlocks(m_pBWT, m_pRevBWT, &obWorkingList, pOBOut);
        obWorkingList.clear();
    }
    else
    {
        pOBOut->splice(pOBOut->end(), obWorkingList);
        assert(obWorkingList.empty());
    }
    // Match the prefix of seq to suffixes
    findOverlapBlocksExact(reverseComplement(seq), m_pBWT, m_pRevBWT, sufSufAF, &obWorkingList, pOBOut, result);
    findOverlapBlocksExact(reverse(seq), m_pRevBWT, m_pBWT, preSufAF, &obWorkingList, pOBOut, result);

    //
    if(m_bIrreducible)
    {
        computeIrreducibleBlocks(m_pBWT, m_pRevBWT, &obWorkingList, pOBOut);
        obWorkingList.clear();
    }
    else
    {
        pOBOut->splice(pOBOut->end(), obWorkingList);
        assert(obWorkingList.empty());
    }

    return result;
}

// Write overlap results to an ASQG file
void OverlapAlgorithm::writeResultASQG(std::ostream& writer, const SeqRecord& read, const OverlapResult& result) const
{
    ASQG::VertexRecord record(read.id, read.seq.toString());
    record.setSubstringTag(result.isSubstring);
    record.write(writer);
}

// Write overlap blocks out to a file
void OverlapAlgorithm::writeOverlapBlocks(std::ostream& writer, size_t readIdx, const OverlapBlockList* pList) const
{
    // Write the hits to the file
    if(!pList->empty())
    {
        // Write the header info
        size_t numBlocks = pList->size();
        writer << readIdx << " " << numBlocks << " ";
        //std::cout << "<Wrote> idx: " << count << " count: " << numBlocks << "\n";
        for(OverlapBlockList::const_iterator iter = pList->begin(); iter != pList->end(); ++iter)
        {
            writer << *iter << " ";
        }
        writer << "\n";
    }
}

// Calculate the ranges in pBWT that contain a prefix of at least minOverlap basepairs that
// overlaps with a suffix of w. The ranges are added to the pOBList
void OverlapAlgorithm::findOverlapBlocksExact(const std::string& w, const BWT* pBWT,
                                                const BWT* pRevBWT, const AlignFlags& af, 
                                              OverlapBlockList* pOBList, OverlapBlockList* pOBFinal, 
                                              OverlapResult& result) const
{
    // All overlaps are added to this list and then sub-maximal overlaps are removed
    OverlapBlockList workingList;

    // The algorithm is as follows:
    // We perform a backwards search using the FM-index for the string w.
    // As we perform the search we collect the intervals 
    // of the significant prefixes (len >= minOverlap) that overlap w.
    BWTIntervalPair ranges;
    size_t l = w.length();
    int start = l - 1;
    BWTAlgorithms::initIntervalPair(ranges, w[start], pBWT, pRevBWT);
    
    // Collect the OverlapBlocks
    for(size_t i = start - 1; i >= 1; --i)
    {
        // Compute the range of the suffix w[i, l]
        BWTAlgorithms::updateBothL(ranges, w[i], pBWT);
        size_t overlapLen = l - i;
        if(overlapLen >= m_minOverlap)
        {
            // Calculate which of the prefixes that match w[i, l] are terminal
            // These are the proper prefixes (they are the start of a read)
            BWTIntervalPair probe = ranges;
            BWTAlgorithms::updateBothL(probe, '$', pBWT);
            
            // The probe interval contains the range of proper prefixes
            if(probe.interval[1].isValid())
            {
                assert(probe.interval[1].lower > 0);
                workingList.push_back(OverlapBlock(probe, overlapLen, 0, af));
            }
        }
    }

    // Determine if this sequence is contained and should not be processed further
    BWTAlgorithms::updateBothL(ranges, w[0], pBWT);

    // Ranges now holds the interval for the full-length read
    // To handle containments, we output the overlapBlock to the final overlap block list
    // and it will be processed later
    // Two possible containment cases:
    // 1) This read is a substring of some other read
    // 2) This read is identical to some other read
    
    // Case 1 is indicated by the existance of a non-$ left or right hand extension
    // In this case we return no alignments for the string
    AlphaCount left_ext = BWTAlgorithms::getExtCount(ranges.interval[0], pBWT);
    AlphaCount right_ext = BWTAlgorithms::getExtCount(ranges.interval[1], pRevBWT);
    if(left_ext.hasDNAChar() || right_ext.hasDNAChar())
    {
        result.isSubstring = true;
    }
    else
    {
        BWTAlgorithms::updateBothL(ranges, '$', pBWT);
        if(ranges.isValid())
            pOBFinal->push_back(OverlapBlock(ranges, w.length(), 0, af));
    }

    // Remove sub-maximal OverlapBlocks and move the remainder to the output list
    removeSubMaximalBlocks(&workingList);
    pOBList->splice(pOBList->end(), workingList);
    return;
}

// Seeded blockwise BWT alignment of prefix-suffix for reads
// Each alignment is given a seed region and a block region
// The seed region is the terminal portion of w where maxDiff + 1 seeds are created
// at least 1 of these seeds must align exactly for there to be an alignment with 
// at most maxDiff differences between the prefix/suffix. Only alignments within the
// range [block_start, block_end] are output. The block_end coordinate is inclusive
void OverlapAlgorithm::findOverlapBlocksInexact(const std::string& w, const BWT* pBWT, 
                                                const BWT* pRevBWT, const AlignFlags& af, 
                                                OverlapBlockList* pOBList, OverlapBlockList* pOBFinal, 
                                                OverlapResult& result) const
{
    int len = w.length();
    int overlap_region_left = len - m_minOverlap;
    SearchSeedVector* pCurrVector = new SearchSeedVector;
    SearchSeedVector* pNextVector = new SearchSeedVector;
    OverlapBlockList partialWorkingList;
    OverlapBlockList fullWorkingList;
    SearchSeedVector::iterator iter;

    // Create and extend the initial seeds
    int actual_seed_length = m_seedLength;
    int actual_seed_stride = m_seedStride;
    if(actual_seed_length == 0)
    {
        // Calculate a seed length and stride that will guarantee all overlaps
        // with error rate m_errorRate will be found
        calculateSeedParameters(w, actual_seed_length, actual_seed_stride);
    }

    createSearchSeeds(w, pBWT, pRevBWT, actual_seed_length, actual_seed_stride, pCurrVector);
    extendSeedsExactRight(w, pBWT, pRevBWT, ED_RIGHT, pCurrVector, pNextVector);
    pCurrVector->clear();
    pCurrVector->swap(*pNextVector);
    assert(pNextVector->empty());

    int num_steps = 0;

    // Perform the inexact extensions
    while(!pCurrVector->empty())
    {
        iter = pCurrVector->begin();
        while(iter != pCurrVector->end())
        {
            SearchSeed& align = *iter;

            // If the current aligned region is right-terminal
            // and the overlap is greater than minOverlap, try to find overlaps
            // or containments
            if(align.right_index == len - 1)
            {
                double align_error = align.calcErrorRate();

                // Check for overlaps
                if(align.left_index <= overlap_region_left && isErrorRateAcceptable(align_error, m_errorRate))
                {
                    int overlapLen = len - align.left_index;
                    BWTIntervalPair probe = align.ranges;
                    BWTAlgorithms::updateBothL(probe, '$', pBWT);
                    
                    // The probe interval contains the range of proper prefixes
                    if(probe.interval[1].isValid())
                    {
                        assert(probe.interval[1].lower > 0);
                        OverlapBlock nBlock(OverlapBlock(probe, overlapLen, align.z, af, align.historyLink->getHistoryVector()));
                        if(overlapLen == len)
                            fullWorkingList.push_back(nBlock);
                        else
                            partialWorkingList.push_back(nBlock);
                    }
                }

                // Check for containments
                // If the seed is left-terminal and there are [ACGT] left/right extensions of the sequence
                // this read must be a substring of another read
                if(align.left_index == 0)
                {
                    AlphaCount left_ext = BWTAlgorithms::getExtCount(align.ranges.interval[0], pBWT);
                    AlphaCount right_ext = BWTAlgorithms::getExtCount(align.ranges.interval[1], pRevBWT);
                    if(left_ext.hasDNAChar() || right_ext.hasDNAChar())
                        result.isSubstring = true;
                }
            }

            // Extend the seed to the right/left
            if(align.dir == ED_RIGHT)
                extendSeedInexactRight(align, w, pBWT, pRevBWT, pNextVector);
            else
                extendSeedInexactLeft(align, w, pBWT, pRevBWT, pNextVector);
            ++iter;
            //pCurrVector->erase(iter++);
        }
        pCurrVector->clear();
        assert(pCurrVector->empty());
        pCurrVector->swap(*pNextVector);

        // Remove identical seeds after we have performed seed_len steps
        // as there is now the chance of identical seeds
        if(num_steps % actual_seed_stride == 0)
        {
            std::sort(pCurrVector->begin(), pCurrVector->end(), SearchSeed::compareLeftRange);
            SearchSeedVector::iterator end_iter = std::unique(pCurrVector->begin(), pCurrVector->end(), 
                                                                   SearchSeed::equalLeftRange);
            pCurrVector->resize(end_iter - pCurrVector->begin());
        }
        ++num_steps;
    }
    WARN_ONCE("TODO: Collect all blocks in the same list then split AFTER removing submaximal");

    // parse the full working list, which has containment overlaps
    removeSubMaximalBlocks(&fullWorkingList);
    pOBFinal->splice(pOBFinal->end(), fullWorkingList);

    // parse the partial block working list, which has the proper overlaps
    removeSubMaximalBlocks(&partialWorkingList);
    pOBList->splice(pOBList->end(), partialWorkingList);

    delete pCurrVector;
    delete pNextVector;
}

struct HashIP  
{                                                                                           
    size_t operator()( const SearchSeed& x ) const
    {
        return std::tr1::hash< int64_t >()( x.ranges.interval[0].lower ^ x.ranges.interval[0].upper );                                                                                   
    }                                                                                        
};                                                                                          

typedef std::tr1::unordered_set<SearchSeed, HashIP> IntervalHash;

// Seeded blockwise BWT alignment of prefix-suffix for reads
// Each alignment is given a seed region and a block region
// The seed region is the terminal portion of w where maxDiff + 1 seeds are created
// at least 1 of these seeds must align exactly for there to be an alignment with 
// at most maxDiff differences between the prefix/suffix. Only alignments within the
// range [block_start, block_end] are output. The block_end coordinate is inclusive
void OverlapAlgorithm::findOverlapBlocksInexactQueue(const std::string& w, const BWT* pBWT, 
                                                const BWT* pRevBWT, const AlignFlags& af, 
                                                OverlapBlockList* pOBList, OverlapBlockList* pOBFinal, 
                                                OverlapResult& result) const
{
    int len = w.length();
    int overlap_region_left = len - m_minOverlap;
    SearchSeedVector* pInitialVector = new SearchSeedVector;
    SearchSeedQueue* pQueue = new SearchSeedQueue;
    IntervalHash* pSeenHash = new IntervalHash;

    OverlapBlockList partialWorkingList;
    OverlapBlockList fullWorkingList;
    SearchSeedVector::iterator iter;

    // Create and extend the initial seeds
    int actual_seed_length = m_seedLength;
    int actual_seed_stride = m_seedStride;
    if(actual_seed_length == 0)
    {
        // Calculate a seed length and stride that will guarantee all overlaps
        // with error rate m_errorRate will be found
        calculateSeedParameters(w, actual_seed_length, actual_seed_stride);
    }
    createSearchSeeds(w, pBWT, pRevBWT, actual_seed_length, actual_seed_stride, pInitialVector);
    extendSeedsExactRightQueue(w, pBWT, pRevBWT, ED_RIGHT, pInitialVector, pQueue);
    
    // Perform the inexact extensions
    while(!pQueue->empty())
    {
        SearchSeed& align = pQueue->front();
        bool valid = true;
        while(valid)
        {
            // Check if the left interval has been seen before
            // If it has the seed is redundant and can be discared
            if(align.length() % actual_seed_stride == 0)
            {
                if(pSeenHash->count(align) > 0)
                {
                    valid = false;
                    break;
                }
                else
                {
                    pSeenHash->insert(align);
                }
            }

            // If the current aligned region is right-terminal
            // and the overlap is greater than minOverlap, try to find overlaps
            // or containments
            if(align.right_index == len - 1)
            {
                double align_error = align.calcErrorRate();

                // Check for overlaps
                if(align.left_index <= overlap_region_left && isErrorRateAcceptable(align_error, m_errorRate))
                {
                    int overlapLen = len - align.left_index;
                    BWTIntervalPair probe = align.ranges;
                    BWTAlgorithms::updateBothL(probe, '$', pBWT);
                    
                    // The probe interval contains the range of proper prefixes
                    if(probe.interval[1].isValid())
                    {
                        assert(probe.interval[1].lower > 0);
                        OverlapBlock nBlock(OverlapBlock(probe, overlapLen, align.z, af, align.historyLink->getHistoryVector()));
                        if(overlapLen == len)
                            fullWorkingList.push_back(nBlock);
                        else
                            partialWorkingList.push_back(nBlock);
                    }
                }

                // Check for containments
                // If the seed is left-terminal and there are [ACGT] left/right extensions of the sequence
                // this read must be a substring of another read
                if(align.left_index == 0)
                {
                    AlphaCount left_ext = BWTAlgorithms::getExtCount(align.ranges.interval[0], pBWT);
                    AlphaCount right_ext = BWTAlgorithms::getExtCount(align.ranges.interval[1], pRevBWT);
                    if(left_ext.hasDNAChar() || right_ext.hasDNAChar())
                        result.isSubstring = true;
                }
            }

            // Extend the seed to the right/left
            if(align.dir == ED_RIGHT)
            {
                if(align.allowMismatches())
                    branchSeedRight(align, w, pBWT, pRevBWT, pQueue);                
                valid = extendSeedExactRight(align, w, pBWT, pRevBWT);
            }
            else
            {
                if(align.allowMismatches())
                    branchSeedLeft(align, w, pBWT, pRevBWT, pQueue);                
                valid = extendSeedExactLeft(align, w, pBWT, pRevBWT);
            }
        }
        pQueue->pop();
    }
    WARN_ONCE("TODO: Collect all blocks in the same list then split AFTER removing submaximal");

    // parse the full working list, which has containment overlaps
    removeSubMaximalBlocks(&fullWorkingList);
    pOBFinal->splice(pOBFinal->end(), fullWorkingList);

    // parse the partial block working list, which has the proper overlaps
    removeSubMaximalBlocks(&partialWorkingList);
    pOBList->splice(pOBList->end(), partialWorkingList);
    
    delete pInitialVector;
    delete pQueue;
    delete pSeenHash;
}

// Calculate the seed length and stride to ensure that we will find all 
// all overlaps with error rate at most m_errorRate.
// To overlap two reads allowing for d errors, we create d+1 seeds so that at least one seed will match
// exactly. d is a function of the overlap length so we define the seed length using the minimum overlap
// parameter. We then tile seeds across the read starting from the end such that for every overlap length
// x, there are at least floor(x * error_rate) + 1 seeds.
void OverlapAlgorithm::calculateSeedParameters(const std::string& w, int& seed_length, int& seed_stride) const
{
    int read_len = w.length();
    seed_length = 0;
    
    // The maximum possible number of differences occurs for a fully-aligned read
    int max_diff_high = static_cast<int>(m_errorRate * read_len);

    // Calculate the seed length to use
    // If the error rate is so low that no differences are possible just seed
    // over the entire minOverlap region
    if(max_diff_high > 0)
    {
        // Calculate the maximum number of differences between two sequences that overlap
        // by minOverlap
        int max_diff_low = static_cast<int>(m_errorRate * m_minOverlap);

         if(max_diff_low == 0)
            max_diff_low = 1;
         
         int seed_region_length = static_cast<int>(ceil(max_diff_low / m_errorRate));
         int num_seeds_low = max_diff_low + 1;
         seed_length = static_cast<int>(seed_region_length / num_seeds_low);
         if(seed_length > static_cast<int>(m_minOverlap))
            seed_length = m_minOverlap;
    }
    else
    {
        seed_length = m_minOverlap;
    }
    seed_stride = seed_length;    
}

// Create and intialize the search seeds
int OverlapAlgorithm::createSearchSeeds(const std::string& w, const BWT* pBWT, 
                                        const BWT* pRevBWT, int seed_length, int seed_stride,
                                        SearchSeedVector* pOutVector) const
{
    // Start a new chain of history links
    SearchHistoryLink rootLink = SearchHistoryNode::createRoot();

    // The maximum possible number of differences occurs for a fully-aligned read
    int read_len = w.length();
    int max_diff_high = static_cast<int>(m_errorRate * read_len);
    static int once = 1;
    if(once)
    {
        printf("Using seed length %d, seed stride %d, max diff %d\n", seed_length, seed_stride, max_diff_high);
        once = 0;
    }    

    // Start the seeds at the end of the read
    int seed_start = read_len - seed_length;

    while(seed_start >= 0)
    {
        SearchSeed seed;
        seed.left_index = seed_start;
        seed.right_index = seed_start;
        seed.dir = ED_RIGHT;
        seed.seed_len = seed_length;
        seed.z = 0;
        seed.maxDiff = max_diff_high;
        seed.historyLink = rootLink;

        // Initialize the left and right suffix array intervals
        char b = w[seed.left_index];
        BWTAlgorithms::initIntervalPair(seed.ranges, b, pBWT, pRevBWT);        
        pOutVector->push_back(seed);

        seed_start -= seed_stride;
        
        // Only create one seed in the exact case
        if(max_diff_high == 0)
            break;
    }
    return seed_length;
}

// Extend all the seeds in pInVector to the right over the entire seed range
void OverlapAlgorithm::extendSeedsExactRightQueue(const std::string& w, const BWT* /*pBWT*/, const BWT* pRevBWT,
                                             ExtendDirection /*dir*/, const SearchSeedVector* pInVector, 
                                             SearchSeedQueue* pOutQueue) const
{
    for(SearchSeedVector::const_iterator iter = pInVector->begin(); iter != pInVector->end(); ++iter)
    {
        SearchSeed align = *iter;
        bool valid = true;
        while(align.isSeed())
        {
            ++align.right_index;
            char b = w[align.right_index];
            BWTAlgorithms::updateBothR(align.ranges, b, pRevBWT);
            if(!align.isIntervalValid(RIGHT_INT_IDX))
            {
                valid = false;
                break;
            }
        }

        if(valid)
            pOutQueue->push(align);
    }
}

// Extend all the seeds in pInVector to the right over the entire seed range
void OverlapAlgorithm::extendSeedsExactRight(const std::string& w, const BWT* /*pBWT*/, const BWT* pRevBWT,
                                             ExtendDirection /*dir*/, const SearchSeedVector* pInVector, 
                                             SearchSeedVector* pOutVector) const
{
    for(SearchSeedVector::const_iterator iter = pInVector->begin(); iter != pInVector->end(); ++iter)
    {
        SearchSeed align = *iter;
        bool valid = true;
        while(align.isSeed())
        {
            ++align.right_index;
            char b = w[align.right_index];
            BWTAlgorithms::updateBothR(align.ranges, b, pRevBWT);
            if(!align.isIntervalValid(RIGHT_INT_IDX))
            {
                valid = false;
                break;
            }
        }

        //std::cout << "Initial seed: ";
        //align.print(w);

        if(valid)
            pOutVector->push_back(align);
    }
}

//
void OverlapAlgorithm::extendSeedInexactRight(SearchSeed& seed, const std::string& w, const BWT* /*pBWT*/, 
                                              const BWT* pRevBWT, SearchSeedVector* pOutVector) const
{
    // If this alignment has run all the way to the end of the sequence
    // switch it to be a left extension sequence
    if(seed.right_index == int(w.length() - 1))
    {
        seed.dir = ED_LEFT;
        pOutVector->push_back(seed);
        return;
    }

    ++seed.right_index;
    
    if(!seed.allowMismatches())
    {
        char b = w[seed.right_index];
        BWTAlgorithms::updateBothR(seed.ranges, b, pRevBWT);
        if(seed.isIntervalValid(RIGHT_INT_IDX))
            pOutVector->push_back(seed);
    }
    else
    {
        for(int i = 0; i < 4; ++i)
        {
            char b = ALPHABET[i];    
            BWTIntervalPair probe = seed.ranges;
            BWTAlgorithms::updateBothR(probe, b, pRevBWT);
            if(probe.interval[RIGHT_INT_IDX].isValid())
            {
                SearchSeed branched = seed;
                branched.ranges = probe;
                if(b != w[seed.right_index])
                {
                    ++branched.z;
                    branched.historyLink = seed.historyLink->createChild(w.length() - seed.right_index, b);
                }
                pOutVector->push_back(branched);
            }
        }
    }
}

//
void OverlapAlgorithm::extendSeedInexactLeft(SearchSeed& seed, const std::string& w, 
                                             const BWT* pBWT, const BWT* /*pRevBWT*/,
                                             SearchSeedVector* pOutVector) const
{
    //printf("ProcessingLEFT: "); align.print(w);
    --seed.left_index;
    if(seed.left_index >= 0)
    {
        if(!seed.allowMismatches())
        {
            // Extend exact
            char b = w[seed.left_index];
            BWTAlgorithms::updateBothL(seed.ranges, b, pBWT);
            if(seed.isIntervalValid(LEFT_INT_IDX))
                pOutVector->push_back(seed);
        }
        else
        {
            for(int i = 0; i < 4; ++i)
            {
                char b = ALPHABET[i];
                BWTIntervalPair probe = seed.ranges;
                BWTAlgorithms::updateBothL(probe, b, pBWT);
                if(probe.interval[LEFT_INT_IDX].isValid())
                {
                    SearchSeed branched = seed;
                    branched.ranges = probe;
                    if(b != w[seed.left_index])
                    {
                        ++branched.z;
                        // The history coordinates are wrt the right end of the read
                        // so that each position corresponds to the length of the overlap
                        // including that position        
                        branched.historyLink = seed.historyLink->createChild(w.length() - seed.left_index, b);                
                        //branched.history.add(w.length() - seed.left_index, b);
                    }
                    pOutVector->push_back(branched);
                }
            }
        }
    }
}

// Extend the seed to the right, return true if the seed is still a valid range
bool OverlapAlgorithm::extendSeedExactRight(SearchSeed& seed, const std::string& w, const BWT* /*pBWT*/, const BWT* pRevBWT) const
{
    // If this alignment has run all the way to the end of the sequence
    // switch it to be a left extension sequence
    if(seed.right_index == int(w.length() - 1))
    {
        seed.dir = ED_LEFT;
        return true;
    }

    ++seed.right_index;
    char b = w[seed.right_index];
    BWTAlgorithms::updateBothR(seed.ranges, b, pRevBWT);
    if(seed.isIntervalValid(RIGHT_INT_IDX))
        return true;
    else
        return false;
}

//
bool OverlapAlgorithm::extendSeedExactLeft(SearchSeed& seed, const std::string& w, const BWT* pBWT, const BWT* /*pRevBWT*/) const
{
    --seed.left_index;
    if(seed.left_index >= 0)
    {
        char b = w[seed.left_index];
        BWTAlgorithms::updateBothL(seed.ranges, b, pBWT);
        if(seed.isIntervalValid(LEFT_INT_IDX))
            return true;
        else
            return false;
    }
    else
    {
        return false;
    }
}

//
void OverlapAlgorithm::branchSeedRight(const SearchSeed& seed, const std::string& w, const BWT* /*pBWT*/, const BWT* pRevBWT, SearchSeedQueue* pQueue) const
{
    int index = seed.right_index + 1;
    
    // Do not branch if the seed is terminal
    if(index == static_cast<int>(w.length()))
        return;

    char c = w[index];
    for(int i = 0; i < 4; ++i)
    {
        char b = ALPHABET[i];
        if(b != c)
        {
            BWTIntervalPair probe = seed.ranges;
            BWTAlgorithms::updateBothR(probe, b, pRevBWT);
            if(probe.interval[RIGHT_INT_IDX].isValid())
            {
                SearchSeed branched = seed;
                branched.right_index = index;
                branched.ranges = probe;
                ++branched.z;
                // The history coordinates are wrt the right end of the read
                // so that each position corresponds to the length of the overlap
                // including that position
                branched.historyLink = seed.historyLink->createChild(w.length() - index, b);
                //branched.history.add(w.length() - index, b);
                pQueue->push(branched);
            }
        }
    }
}

//
void OverlapAlgorithm::branchSeedLeft(const SearchSeed& seed, const std::string& w, const BWT* pBWT, const BWT* /*pRevBWT*/, SearchSeedQueue* pQueue) const
{
    int index = seed.left_index - 1;

    // Do not branch if the seed is terminal
    if(index < 0)
        return;

    char c = w[index];
    for(int i = 0; i < 4; ++i)
    {
        char b = ALPHABET[i];
        if(b != c)
        {
            BWTIntervalPair probe = seed.ranges;
            BWTAlgorithms::updateBothL(probe, b, pBWT);
            if(probe.interval[LEFT_INT_IDX].isValid())
            {
                SearchSeed branched = seed;
                branched.ranges = probe;
                branched.left_index = index;
                ++branched.z;
                // The history coordinates are wrt the right end of the read
                // so that each position corresponds to the length of the overlap
                // including that position
                branched.historyLink = seed.historyLink->createChild(w.length() - index, b);                        
                //branched.history.add(w.length() - index, b);
                pQueue->push(branched);
            }
        }
    }
}

// Calculate the irreducible blocks from the vector of OverlapBlocks
void OverlapAlgorithm::computeIrreducibleBlocks(const BWT* pBWT, const BWT* pRevBWT, 
                                                OverlapBlockList* pOBList, 
                                                OverlapBlockList* pOBFinal) const
{
    // processIrreducibleBlocks requires the pOBList to be sorted in descending order
    pOBList->sort(OverlapBlock::sortSizeDescending);
    _processIrreducibleBlocksInexact(pBWT, pRevBWT, *pOBList, pOBFinal);
}

// iterate through obList and determine the overlaps that are irreducible. This function is recursive.
// The final overlap blocks corresponding to irreducible overlaps are written to pOBFinal.
// Invariant: the blocks are ordered in descending order of the overlap size so that the longest overlap is first.
// Invariant: each block corresponds to the same extension of the root sequence w.
void OverlapAlgorithm::_processIrreducibleBlocksExact(const BWT* pBWT, const BWT* pRevBWT, 
                                                      OverlapBlockList& obList, 
                                                      OverlapBlockList* pOBFinal) const
{
    if(obList.empty())
        return;
    
    // Count the extensions in the top level (longest) blocks first
    int topLen = obList.front().overlapLen;
    AlphaCount ext_count;
    OBLIter iter = obList.begin();
    while(iter != obList.end() && iter->overlapLen == topLen)
    {
        ext_count += iter->getCanonicalExtCount(pBWT, pRevBWT);
        ++iter;
    }
    
    // Three cases:
    // 1) The top level block has ended as it contains the extension $. Output TLB and end.
    // 2) There is a singular unique extension base for all the blocks. Update all blocks and recurse.
    // 3) There are multiple extension bases, branch and recurse.
    // If some block other than the TLB ended, it must be contained within the TLB and it is not output
    // or considered further. 
    // Likewise if multiple distinct strings in the TLB ended, we only output the top one. The rest
    // must have the same sequence as the top one and are hence considered to be contained with the top element.
    if(ext_count.get('$') > 0)
    {
        // An irreducible overlap has been found. It is possible that there are two top level blocks
        // (one in the forward and reverse direction). Since we can't decide which one
        // contains the other at this point, we output hits to both. Under a fixed 
        // length string assumption one will be contained within the other and removed later.
        OBLIter tlbIter = obList.begin();
        while(tlbIter != obList.end() && tlbIter->overlapLen == topLen)
        {
            // Ensure the tlb is actually terminal and not a substring block
            AlphaCount test_count = tlbIter->getCanonicalExtCount(pBWT, pRevBWT);
            assert(test_count.get('$') > 0);
            pOBFinal->push_back(OverlapBlock(*tlbIter));
            ++tlbIter;
        } 
        return;
    }
    else
    {
        // Count the rest of the blocks
        while(iter != obList.end())
        {
            ext_count += iter->getCanonicalExtCount(pBWT, pRevBWT);
            ++iter;
        }

        if(ext_count.hasUniqueDNAChar())
        {

            // Update all the blocks using the unique extension character
            // This character is in the canonical representation wrt to the query
            char b = ext_count.getUniqueDNAChar();
            updateOverlapBlockRangesRight(pBWT, pRevBWT, obList, b);
            return _processIrreducibleBlocksExact(pBWT, pRevBWT, obList, pOBFinal);
        }
        else
        {
            for(size_t idx = 0; idx < DNA_ALPHABET_SIZE; ++idx)
            {
                char b = ALPHABET[idx];
                if(ext_count.get(b) > 0)
                {
                    OverlapBlockList branched = obList;
                    updateOverlapBlockRangesRight(pBWT, pRevBWT, branched, b);
                    _processIrreducibleBlocksExact(pBWT, pRevBWT, branched, pOBFinal);
                }
            }
        }
    }
}

// Classify the blocks in obList as irreducible, transitive or substrings. The irreducible blocks are
// put into pOBFinal. The remaining are discarded.
// Invariant: the blocks are ordered in descending order of the overlap size so that the longest overlap is first.
void OverlapAlgorithm::_processIrreducibleBlocksInexact(const BWT* pBWT, const BWT* pRevBWT, 
                                                        OverlapBlockList& activeList, 
                                                        OverlapBlockList* pOBFinal) const
{
    if(activeList.empty())
        return;
    
    // The activeList contains all the blocks that are not yet right terminal

    // Count the extensions in the top level (longest) blocks first
    bool all_eliminated = false;
    while(!activeList.empty() && !all_eliminated)
    {
        // The terminalBlock list contains all the blocks that became right-terminal
        // in the current extension round.
        OverlapBlockList terminalList;
        OverlapBlockList potentialContainedList;

        // Perform a single round of extension, any terminal blocks
        // are moved to the terminated list
        extendActiveBlocksRight(pBWT, pRevBWT, activeList, terminalList, potentialContainedList);

        // Compare the blocks in the contained list against the other terminal and active blocks
        // If they are a substring match to any of these, discard them
        OverlapBlockList::iterator containedIter = potentialContainedList.begin();
        for(; containedIter != potentialContainedList.end(); ++containedIter)
        {
           if(!isBlockSubstring(*containedIter, terminalList, m_errorRate) && 
              !isBlockSubstring(*containedIter, activeList, m_errorRate))
           {
                // Not a substring, move to terminal list
                terminalList.push_back(*containedIter);
                //std::cout << "Contained block kept: " << containedIter->overlapLen << "\n";
           }
           else
           {
                //std::cout << "Contained block found and removed: " << containedIter->overlapLen << "\n";
           }
        }

        // Using the terminated blocks, mark as eliminated any active blocks
        // that form a valid overlap to the terminal block. These are transitive edges
        // We do not compare two terminal blocks, we don't consider these overlaps to be
        // transitive
        OverlapBlockList::iterator terminalIter = terminalList.begin();
        for(; terminalIter != terminalList.end(); ++terminalIter)
        {
#ifdef TEMPDEBUG
            std::cout << "***TLB of length " << terminalIter->overlapLen << " has ended\n";
#endif       
            all_eliminated = true;
            OverlapBlockList::iterator activeIter = activeList.begin();
            for(; activeIter != activeList.end(); ++activeIter)
            {
                if(activeIter->isEliminated)
                    continue; // skip previously marked blocks

                double inferredErrorRate = calculateBlockErrorRate(*terminalIter, *activeIter);
                if(isErrorRateAcceptable(inferredErrorRate, m_errorRate))
                {
#ifdef TEMPDEBUG                            
                    std::cout << "Marking block of length " << activeIter->overlapLen << " as eliminated\n";
#endif
                    activeIter->isEliminated = true;
                }
                else
                {
                    all_eliminated = false;
                }
            } 
            
            // Move this block to the final list if it has not been previously marked eliminated
            if(!terminalIter->isEliminated)
            {
#ifdef TEMPDEBUG
                std::cout << "Adding block of length " << terminalIter->overlapLen << " to final\n";
#endif                
                pOBFinal->push_back(*terminalIter);
            }
        }
    }

    activeList.clear();
}

// Extend all the blocks in activeList by one base to the right
// Move all right-terminal blocks to the termainl list. If a block 
// is terminal and potentially contained by another block, add it to 
// containedList
void OverlapAlgorithm::extendActiveBlocksRight(const BWT* pBWT, const BWT* pRevBWT, 
                                               OverlapBlockList& activeList, 
                                               OverlapBlockList& terminalList,
                                               OverlapBlockList& containedList) const
{
    int longestOverlap = activeList.front().overlapLen;
    OverlapBlockList::iterator iter = activeList.begin();
    OverlapBlockList::iterator next;
    while(iter != activeList.end())
    {
        next = iter;
        ++next;

        // Check if block is terminal
        AlphaCount ext_count = iter->getCanonicalExtCount(pBWT, pRevBWT);
        if(ext_count.get('$') > 0)
        {
            if(iter->overlapLen == longestOverlap || true)
            {
                terminalList.push_back(*iter);
#ifdef TEMPDEBUG            
            std::cout << "Block of length " << iter->overlapLen << " moved to terminal\n";
#endif
            }
            else
            {
#ifdef TEMPDEBUG            
            std::cout << "Block of length " << iter->overlapLen << " moved to contained\n";
#endif
               
                containedList.push_back(*iter);
            }
        }

        int curr_extension = iter->forwardHistory.size();

        // Perform the right extensions
        for(size_t idx = 0; idx < DNA_ALPHABET_SIZE; ++idx)
        {
            OverlapBlock branched = *iter;
            char b = ALPHABET[idx];
            char cb = iter->flags.isQueryComp() ? complement(b) : b;
            BWTAlgorithms::updateBothR(branched.ranges, cb, branched.getExtensionBWT(pBWT, pRevBWT));

            if(branched.ranges.isValid())
            {
                branched.forwardHistory.add(curr_extension, b);
                // Insert the new block after the iterator
                activeList.insert(iter, branched);
            }
        }

        // All extensions of iter have been made, remove it from the list
        activeList.erase(iter);
        iter = next; // this skips the newly-inserted blocks
    }
} 

// Return true if the terminalBlock is a substring of any member of blockList
bool OverlapAlgorithm::isBlockSubstring(OverlapBlock& terminalBlock, const OverlapBlockList& blockList, double maxER) const
{
    OverlapBlockList::const_iterator iter = blockList.begin();
    size_t right_extension_length = terminalBlock.forwardHistory.size();
    for(; iter != blockList.end(); ++iter)
    {
        if(terminalBlock.overlapLen == iter->overlapLen && 
           right_extension_length == iter->forwardHistory.size())
        {
            continue; // same length, cannot be a substring
        }
        
        // Calculate error rate between blocks
        double er = calculateBlockErrorRate(terminalBlock, *iter);
        if(isErrorRateAcceptable(er, maxER))
            return true;
    }
    return false;
}

// Calculate the error rate between two overlap blocks using their history
double OverlapAlgorithm::calculateBlockErrorRate(const OverlapBlock& terminalBlock, const OverlapBlock& otherBlock) const
{
    int back_max = std::min(terminalBlock.overlapLen, otherBlock.overlapLen);
    int backwards_diff = SearchHistoryVector::countDifferences(terminalBlock.backHistory, otherBlock.backHistory, back_max);

    // We compare the forward (right) extension only up to the last position of the terminated block's extension
    int forward_len = terminalBlock.forwardHistory.size();
    int forward_max = forward_len - 1;
    int forward_diff = SearchHistoryVector::countDifferences(terminalBlock.forwardHistory, otherBlock.forwardHistory, forward_max);

    // Calculate the length of the inferred overlap
    int trans_overlap_length = back_max + forward_len;
    double er = static_cast<double>(backwards_diff + forward_diff) / trans_overlap_length;
            
#ifdef TEMPDEBUG
    std::cout << "OL: " << terminalBlock.overlapLen << "\n";
    std::cout << "TLB BH: " << terminalBlock.backHistory << "\n";
    std::cout << "TB  BH: " << otherBlock.backHistory << "\n";
    std::cout << "TLB FH: " << terminalBlock.forwardHistory << "\n";
    std::cout << "TB  FH: " << otherBlock.forwardHistory << "\n";
    std::cout << "BM: " << back_max << " FM: " << forward_max << "\n";
    std::cout << "IOL: " << trans_overlap_length << " TD: " << (backwards_diff + forward_diff) << "\n";
    std::cout << "Block of length " << otherBlock.overlapLen << " has ier: " << er << "\n";
#endif
    return er;
}

// Update the overlap block list with a righthand extension to b, removing ranges that become invalid
void OverlapAlgorithm::updateOverlapBlockRangesRight(const BWT* pBWT, const BWT* pRevBWT, 
                                                     OverlapBlockList& obList, char b) const
{
    OverlapBlockList::iterator iter = obList.begin(); 
    while(iter != obList.end())
    {
        char cb = iter->flags.isQueryComp() ? complement(b) : b;
        BWTAlgorithms::updateBothR(iter->ranges, cb, iter->getExtensionBWT(pBWT, pRevBWT));
        // remove the block from the list if its no longer valid
        if(!iter->ranges.isValid())
            iter = obList.erase(iter);
        else
            ++iter;
    }
}

