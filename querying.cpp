#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <cctype>
#include <cmath>
#include <chrono>
using namespace std;

const double N = 8841823;
const double k1 = 1.2;
const double b = 0.75;
const int k = 10;

struct BlockMetadata
{
    uint32_t lastDocId;
    uint32_t docSize;  // compressed doc size
    uint32_t freqSize; // compressed freq size
};

struct LexiconEntry
{
    uint32_t startBlock; // which block term starts in
    uint32_t startIndex; // which index within block term start (0-127)
    uint32_t listLength; // total postings for the term
};

struct ScoreDoc
{
    double score;
    uint32_t docId;
};

struct MinHeapComp
{ // functor for pq comparator
    bool operator()(const ScoreDoc &a, const ScoreDoc &b) const
    {
        if (a.score == b.score)
        {
            return a.docId > b.docId;
        }
        else
        {
            return a.score > b.score;
        }
    }
};

class ListPointer
{
public:
    ListPointer(const string &term, const LexiconEntry &lexicon) : term(term), listLength(lexicon.listLength), blockNum(lexicon.startBlock), startBlock(lexicon.startBlock), startIndex(lexicon.startIndex)
    {
        uint32_t postingsLeft = (lexicon.listLength > (128 - lexicon.startIndex)) ? (lexicon.listLength - (128 - lexicon.startIndex)) : 0;
        finalBlock = lexicon.startBlock + (postingsLeft + 127) / 128;
    }

    // load 1 block of docIDs and freqs into ListPointer buffers
    void loadBlock(ifstream &ifs, const vector<BlockMetadata> &metadata, const vector<uint64_t> &blockOffsets)
    {
        if (blockNum >= metadata.size())
        {
            return;
        }

        // seek and read compressed bytes from compressed inverted index at offset into buffer
        ifs.seekg(blockOffsets[blockNum], ios::beg);

        // compressed doc bytes
        uint32_t docSize = metadata[blockNum].docSize;
        docBuffer.resize(docSize);
        ifs.read(reinterpret_cast<char *>(docBuffer.data()), docSize);

        // compressed freq bytes
        uint32_t freqSize = metadata[blockNum].freqSize;
        freqBuffer.resize(freqSize);
        ifs.read(reinterpret_cast<char *>(freqBuffer.data()), freqSize);

        docBufPos = 0;
        freqBufPos = 0;
        prevDocId = 0; // reset delta base when new block starts

        // skip docIDs before startIndex
        if (blockNum == startBlock && startIndex > 0)
        {
            for (uint32_t i = 0; i < startIndex; ++i)
            {
                uint32_t gap = varbyteDecode(docBuffer, docBufPos);
                prevDocId += gap;
                varbyteDecode(freqBuffer, freqBufPos);
            }
        }

        currentPos = 0;
    }

    uint32_t nextGEQ(uint32_t targetDoc, ifstream &ifs, const vector<BlockMetadata> &metadata, const vector<uint64_t> &blockOffsets)
    {
        if (currentPos >= listLength)
        { // exhausted this term's postings
            return UINT32_MAX;
        }

        // linear decoding one by one
        while (true)
        {
            if (docBufPos >= docBuffer.size()) // need new block
            {
                if (++blockNum > finalBlock || blockNum >= metadata.size())
                    return UINT32_MAX;
                loadBlock(ifs, metadata, blockOffsets);
            }

            uint32_t gap = varbyteDecode(docBuffer, docBufPos);
            uint32_t doc = prevDocId + gap;
            prevDocId = doc;

            uint32_t freq = varbyteDecode(freqBuffer, freqBufPos);

            ++currentPos;
            currentDoc = doc;
            currentFreq = freq;

            if (doc >= targetDoc)
                return doc;
        }
    }

    double getScore(double docLength, double averageDocLength)
    {
        // BM25
        double logNum = N - listLength + 0.5;
        double logDenom = listLength + 0.5;
        double operand_one = log(logNum / logDenom);

        double normalized_doc = docLength / averageDocLength;
        double big_k = k1 * ((1 - b) + (b * normalized_doc));
        double num = (k1 + 1) * currentFreq;
        double denom = big_k + currentFreq;
        double operand_two = num / denom;

        double score = operand_one * operand_two;
        return score;
    }

    void close()
    {
        docBuffer.clear();
        freqBuffer.clear();
    }

    // needed to get maxscore approx
    uint32_t getListLength() const
    {
        return listLength;
    }

    void setCurrentFrequency(uint32_t val)
    {
        currentFreq = val;
    }

private:
    uint32_t varbyteDecode(const vector<unsigned char> &buf, size_t &pos)
    {
        uint32_t num = 0;
        uint32_t shift = 0;
        uint8_t curr;

        // varbyte is little endian, decode one num at a time
        do
        {
            curr = buf[pos++];
            num += (curr & 127) << shift;
            shift += 7;
        } while (curr >= 128);

        return num;
    }

    // galloping - if many blocks ahead have lastDocId < target, skip exponentially
    // find block by block
    // uint32_t gallopBlock(uint32_t targetDoc, const vector<BlockMetadata> &metadata)
    // {

    // // if curr block's lastDocId >= target, return start Block
    // // if first block to search has lastDocId that already exceeds target, means that target is somewhere in that block
    // if (metadata[blockNum].lastDocId >= targetDoc)
    // {
    //     return blockNum;
    // }

    // uint32_t skip = 1;
    // uint32_t newBlock = blockNum;
    // // exponential skip if valid and lastDocId < targetDoc
    // while ((newBlock + skip <= finalBlock) && (metadata[newBlock + skip].lastDocId < targetDoc))
    // {
    //     newBlock += skip;
    //     skip <<= 1; // double the skips
    // }

    // // reduce skips if went over
    // skip >>= 1;
    // while (skip > 0)
    // {
    //     if ((newBlock + skip <= finalBlock) && (metadata[newBlock + skip].lastDocId < targetDoc))
    //     {
    //         newBlock += skip;
    //     }
    //     skip >>= 1;
    // }

    // if (newBlock < finalBlock && metadata[newBlock].lastDocId < targetDoc)
    // {
    //     return newBlock + 1;
    // }
    // else
    // {
    //     return newBlock;
    // }
    // }

    uint32_t findBlock(uint32_t targetDoc, const vector<BlockMetadata> &metadata)
    {
        uint32_t nextBlock = blockNum;
        while (nextBlock < metadata.size() && nextBlock <= finalBlock && metadata[nextBlock].lastDocId < targetDoc)
        {
            ++nextBlock;
        }

        if (nextBlock > finalBlock || nextBlock >= metadata.size())
        {
            return UINT32_MAX;
        }

        return nextBlock;
    }

    string term;
    uint32_t listLength;     // total postings for term
    uint32_t currentPos = 0; // curr index in postings list
    uint32_t currentDoc;     // most recent decoded docID, updated on nextGEQ
    uint32_t currentFreq;    // freq of term in currentDoc
    uint32_t blockNum;       // index of current COMPRESSED block in file (based on startBlock)
    uint32_t finalBlock;     // prevents galloping from bleeding into next term's postings
    uint32_t startBlock;     // first block where term inverted list starts
    uint32_t startIndex;     // first index offset within start block
    uint32_t prevDocId;      // for delta decoding varbyte
    // buffers for curr block (compressed)
    vector<unsigned char> docBuffer;  // to store compressed bytes from disk, read metadata[blockNum].docSize bytes
    vector<unsigned char> freqBuffer; // to store compressed bytes from disk, read metadata[blockNum].freqSize bytes
    // will decompress byte-by-byte when we need posting

    // position inside compressed buffers (byte offset)
    size_t docBufPos = 0;  // byte position inside current docBuffer, reset to 0 when load new block, indicates how far in buffer decoded, updated when decoding
    size_t freqBufPos = 0; // byte position inside current freqBuffer, reset to 0 when load new block, indicates how far in buffer decoded, updated when decoding
};

vector<uint64_t> computeBlockOffsets(const vector<BlockMetadata> &metadata);
vector<ScoreDoc> conjunctiveDAAT(vector<string> &queryTerms,
                                 const unordered_map<string, size_t> &termToIndex,
                                 ifstream &ifs,
                                 const vector<LexiconEntry> &lexicon,
                                 const vector<BlockMetadata> &metadata,
                                 const vector<uint64_t> &blockOffsets,
                                 const vector<int> &pageTable,
                                 double averageDocLength);
vector<ScoreDoc> disjunctiveDAAT(const vector<string> &queryTerms,
                                 const unordered_map<string, size_t> &termToIndex,
                                 ifstream &ifs,
                                 const vector<LexiconEntry> &lexicon,
                                 const vector<BlockMetadata> &metadata,
                                 const vector<uint64_t> &blockOffsets,
                                 const vector<int> &pageTable,
                                 double averageDocLength);
void cleanTerm(string &term);
vector<int> loadPageTable(ifstream &ifs);
double getAverageDocLength(const vector<int> &pageTable);
vector<LexiconEntry> loadLexicon(ifstream &ifs, unordered_map<string, size_t> &termToIndex);
vector<BlockMetadata> loadMetadata(ifstream &ifs);

int main()
{
    string setting;
    cout << "Conjunctive or Disjunctive (c/d): " << endl;
    cin >> setting;
    cin.ignore(numeric_limits<streamsize>::max(), '\n');
    cout << endl;

    // in case input is invalid, can only choose 1 setting
    while (setting.size() > 1 || (setting != "c" && setting != "d" && setting != "C" && setting != "D"))
    {
        cout << "Type \"c\" or \"d\" only" << endl;
        setting = "";
        cin >> setting;
        cin.ignore(numeric_limits<streamsize>::max(), '\n');
        cout << endl;
    }

    // just in case input is capitalized
    for (char &c : setting)
    {
        c = tolower(c);
    }

    string queryInput;
    cout << "Input Query Terms Separated by Spaces (e.g. \"dog cat fish\"): " << endl;
    getline(cin, queryInput);

    string term;
    vector<string> queryTerms;
    stringstream ss(queryInput);
    while (getline(ss, term, ' '))
    {
        if (term.empty())
        {
            continue;
        }
        cleanTerm(term);
        if (!term.empty())
        {
            queryTerms.push_back(term);
        }
    }

    for (size_t i = 0; i < queryTerms.size(); ++i)
    {
        cout << "Term " << i + 1 << ": " << queryTerms[i] << endl;
    }

    using namespace std::chrono;
    auto startTime = high_resolution_clock::now();

    string indexFilename = "compressed_inverted_index.bin";
    string lexiconFilename = "lexicon.bin";
    string metadataFilename = "metadata.bin";
    string pageTableFilename = "page_table.bin";
    ifstream indexIfs(indexFilename, ios::binary);
    ifstream lexiconIfs(lexiconFilename, ios::binary);
    ifstream metadataIfs(metadataFilename, ios::binary);
    ifstream pageTableIfs(pageTableFilename, ios::binary);

    if (!indexIfs || !lexiconIfs || !metadataIfs || !pageTableIfs)
    {
        cerr << "Failed to open files!" << endl;
        return 1;
    }

    // put page table in memory
    vector<int> pageTable = loadPageTable(pageTableIfs);
    double averageDocLength = getAverageDocLength(pageTable);

    // put lexicon in memory and have mapping from term to index
    unordered_map<string, size_t> termToIndex;
    vector<LexiconEntry> lexicon = loadLexicon(lexiconIfs, termToIndex);

    // process metadata in memory
    vector<BlockMetadata> metadata = loadMetadata(metadataIfs);
    vector<uint64_t> blockOffsets = computeBlockOffsets(metadata);

    vector<ScoreDoc> results;
    if (setting == "c")
    {
        bool allFound = true;
        for (const string &term : queryTerms)
        {
            if (termToIndex.find(term) == termToIndex.end())
            {
                cout << term << " not found in lexicon";
                allFound = false;
                break;
                // if 1 term not found, no results
            }
        }

        if (allFound)
        {
            results = conjunctiveDAAT(queryTerms, termToIndex, indexIfs, lexicon, metadata, blockOffsets, pageTable, averageDocLength);
        }
    }
    else if (setting == "d")
    {
        bool atLeastOneFound = false;
        for (const string &term : queryTerms)
        {
            if (termToIndex.find(term) != termToIndex.end())
            {
                atLeastOneFound = true;
                break;
                // if all terms not found, no results
            }
        }

        if (atLeastOneFound)
        {
            results = disjunctiveDAAT(queryTerms, termToIndex, indexIfs, lexicon, metadata, blockOffsets, pageTable, averageDocLength);
        }
    }

    auto endTime = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(endTime - startTime).count();
    std::cout << "Elapsed time: " << duration << " ms" << std::endl;

    if (results.empty())
    {
        cout << "No documents found for these query terms";
    }

    // prints result in reverse to go from highest to lowest score
    for (size_t i = results.size(); i > 0; --i)
    {
        cout << "Score: " << results[i - 1].score << ", DocID: " << results[i - 1].docId << endl;
    }
}

// compute block offsets once from metadata for each block instead of doing it each time we get a term
vector<uint64_t> computeBlockOffsets(const vector<BlockMetadata> &metadata)
{
    // basically prefix sums
    vector<uint64_t> offsets(metadata.size());
    uint64_t off = 0;
    for (size_t i = 0; i < metadata.size(); ++i)
    {
        offsets[i] = off;
        off += (uint64_t)metadata[i].docSize + (uint64_t)metadata[i].freqSize;
    }
    return offsets;
}

// conjunctive DAAT
vector<ScoreDoc> conjunctiveDAAT(vector<string> &queryTerms,
                                 const unordered_map<string, size_t> &termToIndex,
                                 ifstream &ifs,
                                 const vector<LexiconEntry> &lexicon,
                                 const vector<BlockMetadata> &metadata,
                                 const vector<uint64_t> &blockOffsets,
                                 const vector<int> &pageTable,
                                 double averageDocLength)
{
    size_t numTerms = queryTerms.size();
    vector<ListPointer *> lp(numTerms);

    // sort terms based on length of inverted lists (shortest first)
    sort(queryTerms.begin(), queryTerms.end(), [&](const string &a, const string &b)
         { return lexicon[termToIndex.at(a)].listLength < lexicon[termToIndex.at(b)].listLength; });

    // open all lists
    for (size_t i = 0; i < numTerms; ++i)
    {
        lp[i] = new ListPointer(queryTerms[i], lexicon[termToIndex.at(queryTerms[i])]);
        lp[i]->loadBlock(ifs, metadata, blockOffsets);
    }

    // current docID in each list
    vector<uint32_t> currDoc(numTerms);
    for (size_t i = 0; i < numTerms; ++i)
        currDoc[i] = lp[i]->nextGEQ(0, ifs, metadata, blockOffsets);

    // min-heap for top-k
    priority_queue<ScoreDoc, vector<ScoreDoc>, MinHeapComp> topK;

    while (true)
    {
        // find candidate = max of current docIDs
        uint32_t candidate = 0;
        bool exhausted = false;
        for (size_t i = 0; i < numTerms; ++i)
        {
            if (currDoc[i] == UINT32_MAX)
            {
                exhausted = true;
                break;
            }
            candidate = max(candidate, currDoc[i]);
        }
        if (exhausted)
            break;

        // advance all lists to candidate
        bool allMatch = true;
        for (size_t i = 0; i < numTerms; ++i)
        {
            if (currDoc[i] < candidate)
            {
                currDoc[i] = lp[i]->nextGEQ(candidate, ifs, metadata, blockOffsets);
                if (currDoc[i] == UINT32_MAX)
                {
                    exhausted = true;
                    break;
                }
            }
            if (currDoc[i] != candidate)
                allMatch = false;
        }
        if (exhausted)
            break;

        if (allMatch)
        {
            // compute score
            double score = 0.0;
            for (size_t i = 0; i < numTerms; ++i)
                score += lp[i]->getScore(pageTable[candidate], averageDocLength);

            // maintain top-k heap
            if (topK.size() < k)
            {
                topK.push({score, candidate});
            }
            else if (score > topK.top().score)
            {
                topK.pop();
                topK.push({score, candidate});
            }

            // advance all lists past candidate
            for (size_t i = 0; i < numTerms; ++i)
                currDoc[i] = lp[i]->nextGEQ(candidate + 1, ifs, metadata, blockOffsets);
        }
    }

    for (size_t i = 0; i < lp.size(); ++i)
    {
        lp[i]->close();
        delete lp[i];
    }

    vector<ScoreDoc> results;
    while (!topK.empty())
    {
        results.push_back(topK.top());
        topK.pop();
    }

    return results; // ordered from lowest to highest score
}

vector<ScoreDoc> disjunctiveDAAT(const vector<string> &queryTerms,
                                 const unordered_map<string, size_t> &termToIndex,
                                 ifstream &ifs,
                                 const vector<LexiconEntry> &lexicon,
                                 const vector<BlockMetadata> &metadata,
                                 const vector<uint64_t> &blockOffsets,
                                 const vector<int> &pageTable,
                                 double averageDocLength)
{
    size_t numTerms = queryTerms.size();
    // iterate over union of postings, compute
    vector<ListPointer *> lp(numTerms);

    // open all lists
    for (size_t i = 0; i < numTerms; ++i)
    {
        ListPointer *p = new ListPointer(queryTerms[i], lexicon[termToIndex.at(queryTerms[i])]);
        p->loadBlock(ifs, metadata, blockOffsets);
        lp[i] = p;
    }

    // sort posting lists by max possible impact score to identify essential lists
    // don't want to decode each frequency to find max, so just use listLength to set upper bound
    vector<double> maxScores(numTerms);
    for (size_t i = 0; i < numTerms; ++i)
    {
        // assume highest freq is listLength (max possible freq)
        // approx upper bound for each list
        uint32_t listLength = lp[i]->getListLength();
        lp[i]->setCurrentFrequency(listLength);
        // pageTable[0] arbitrarily chosen for length normalization since don't know "true" docId yet
        maxScores[i] = lp[i]->getScore(pageTable[0], averageDocLength);
    }

    // sort from lowest to highest impact
    // if sum of remaining maxScores (of higher ones) < threshold, can stop early
    vector<size_t> order(numTerms);
    for (size_t i = 0; i < numTerms; ++i)
    {
        order[i] = i;
    }
    sort(order.begin(), order.end(), [&](size_t a, size_t b)
         { return maxScores[a] < maxScores[b]; });

    // keep track of curr docIDs in each list
    vector<uint32_t> currDoc(numTerms);
    for (size_t i = 0; i < numTerms; ++i)
    {
        currDoc[i] = lp[i]->nextGEQ(0, ifs, metadata, blockOffsets);
    }

    // use min heap so we take out minimum out of the top k in constant time
    priority_queue<ScoreDoc, vector<ScoreDoc>, MinHeapComp> topK;

    while (true)
    {
        // find next candidate docID = min of curr docIDs across all term lists
        uint32_t candidate = UINT32_MAX;
        for (size_t i = 0; i < numTerms; ++i)
        {
            if (currDoc[i] != UINT32_MAX)
            {
                candidate = min(candidate, currDoc[i]);
            }
        }
        if (candidate == UINT32_MAX)
        {
            break; // all lists exhausted
        }

        // sum score for candidate
        double score = 0.0;
        double remainingMax = 0.0; // for non-essential early termination, sum of unused maxScores

        for (size_t idx : order)
        {
            // if one of it matches, can add to score, not necessarily all inverted lists need to have it, so we put those in remainingMax
            if (currDoc[idx] == candidate)
            {
                score += lp[idx]->getScore(pageTable[candidate], averageDocLength);

                // advance list to meet >= candidate + 1, so basically next docID
                currDoc[idx] = lp[idx]->nextGEQ(candidate + 1, ifs, metadata, blockOffsets);
            }
            else
            {
                remainingMax += maxScores[idx];
            }
        }

        // early termination: skip non-essential lists if cannot affect topK
        // heap full and if add best possible scores from remaining list, still below threshold, skip it
        if (topK.size() >= k && score + remainingMax <= topK.top().score)
            continue;

        // maintain top-k heap
        if (topK.size() < k)
        {
            topK.push({score, candidate});
        }
        // || (score == topK.top().score && candidate > topK.top().docId) - ignore
        else if (score > topK.top().score)
        {
            topK.pop();
            topK.push({score, candidate});
        }
    }

    for (size_t idx = 0; idx < lp.size(); ++idx)
    {
        lp[idx]->close();
        delete lp[idx];
    }

    vector<ScoreDoc> results;
    while (!topK.empty())
    {
        results.push_back(topK.top());
        topK.pop();
    }
    return results;
}

void cleanTerm(string &term)
{
    string cleaned;
    // removes non-ascii and punctuations
    for (char c : term)
    {
        unsigned char uc = tolower((unsigned char)c);
        if (uc <= 127 && !ispunct(uc) && isalnum(uc))
        {
            cleaned += uc; // valid non-punctuation ascii
        }
    }
    term = cleaned;
}

vector<int> loadPageTable(ifstream &ifs)
{
    vector<int> table;
    int docLength;
    while (ifs.read(reinterpret_cast<char *>(&docLength), sizeof(int)))
    {
        table.push_back(docLength);
    }
    return table;
}

double getAverageDocLength(const vector<int> &pageTable)
{
    uint64_t total = 0;
    for (int docLength : pageTable)
    {
        total += docLength;
    }
    return static_cast<double>(total) / pageTable.size();
}

vector<LexiconEntry> loadLexicon(ifstream &ifs, unordered_map<string, size_t> &termToIndex)
{
    vector<LexiconEntry> lexicon;
    uint32_t termSize;
    while (ifs.read(reinterpret_cast<char *>(&termSize), sizeof(termSize)))
    {
        string term(termSize, '\0');
        ifs.read(&term[0], termSize);

        LexiconEntry entry;
        ifs.read(reinterpret_cast<char *>(&entry), sizeof(LexiconEntry));

        termToIndex[term] = lexicon.size();
        lexicon.push_back(entry);
    }
    return lexicon;
}

vector<BlockMetadata> loadMetadata(ifstream &ifs)
{
    vector<BlockMetadata> metadata;
    BlockMetadata block;
    while (ifs.read(reinterpret_cast<char *>(&block), sizeof(BlockMetadata)))
    {
        metadata.push_back(block);
    }
    return metadata;
}