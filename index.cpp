#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
using namespace std;

const int MAX_BUF_POSTINGS = 128;

struct PostingEntry
{
    string term;
    int docId;
    int freq;
};

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

struct Block
{ // each of size 128 docIds, and 128 freqs

    vector<int> docIds;
    vector<int> freqs; // freq for corresponding docIds

    void clear()
    {
        docIds.clear();
        freqs.clear();
    }
};

// VARBYTE ENCODING
void writeByte(vector<unsigned char> &buffer, uint8_t val) // 8 byte num
{
    buffer.push_back(val);
}

void varbyteEncode(vector<unsigned char> &buffer, uint32_t num)
{
    while (num >= 128)
    {
        writeByte(buffer, 128 + (num & 127)); // set the 1 and then the next 7 bits
        num >>= 7;                            // right shift by 7 bits
    }
    writeByte(buffer, static_cast<uint8_t>(num)); // without the 1 bit at the front
}

// INVERTED INDEX + LEXICON
bool readNextRecord(ifstream &ifs, PostingEntry &p)
{
    uint32_t termLen;
    if (!ifs.read(reinterpret_cast<char *>(&termLen), sizeof(termLen)))
    {
        return false;
    }
    if (termLen == 0)
    {
        return false;
    }

    string term(termLen, '\0');
    if (!ifs.read(&term[0], termLen))
    {
        return false;
    }

    int docId, freq;
    if (!ifs.read(reinterpret_cast<char *>(&docId), sizeof(docId)))
    {
        return false;
    }
    if (!ifs.read(reinterpret_cast<char *>(&freq), sizeof(freq)))
    {
        return false;
    }

    p = PostingEntry{term, docId, freq};
    return true;
}

void writeLexiconEntry(ofstream &ofs, const string &term, LexiconEntry &entry)
{
    uint32_t termSize = term.size();
    ofs.write(reinterpret_cast<char *>(&termSize), sizeof(termSize));
    ofs.write(term.data(), termSize);
    ofs.write(reinterpret_cast<char *>(&entry), sizeof(LexiconEntry));
}

// compress 1 block of docIDs and 1 block of freqs
// append metadata
// increment blockCount
void compressBlock(ofstream &ofs, const Block &block, vector<unsigned char> &buffer, vector<BlockMetadata> &metadata, uint32_t &blockCount)
{
    buffer.clear();
    // compress and write docIds
    // use delta then varbyte !!
    int prevDocId = 0;
    for (int docId : block.docIds)
    {
        uint32_t delta = docId - prevDocId;
        varbyteEncode(buffer, delta);
        prevDocId = docId;
    }
    // write to output buffer
    ofs.write(reinterpret_cast<char *>(buffer.data()), buffer.size());
    uint32_t lastDocId = static_cast<uint32_t>(block.docIds.back());
    uint32_t docByteCount = static_cast<uint32_t>(buffer.size());
    buffer.clear();

    // compress and write freqs
    for (int freq : block.freqs)
    {
        varbyteEncode(buffer, static_cast<uint32_t>(freq));
    }
    ofs.write(reinterpret_cast<char *>(buffer.data()), buffer.size());
    uint32_t freqByteCount = static_cast<uint32_t>(buffer.size());
    buffer.clear();

    // record metadata - one entry per block
    BlockMetadata blockInfo{lastDocId, docByteCount, freqByteCount};
    metadata.push_back(blockInfo);

    ++blockCount;
}

void generateInvertedIndex()
{
    string inFilename = "final_merged.bin";
    string outFilename = "compressed_inverted_index.bin";
    string lexiconFilename = "lexicon.bin";
    string metadataFilename = "metadata.bin";

    ifstream ifs(inFilename, ios::binary);
    if (!ifs)
    {
        cerr << "Failed to open " << inFilename << endl;
        exit(1);
    }

    // inverted index
    ofstream ofs(outFilename, ios::binary);
    ofstream lexicon(lexiconFilename, ios::binary);
    ofstream metadataOut(metadataFilename, ios::binary);

    string currentTerm;
    Block block;
    vector<unsigned char> buffer; // temp buffer for the term docids/freqs
    vector<BlockMetadata> metadata;

    uint32_t blockCount = 0;       // completed blocks
    uint32_t termStartBlock = 0;   // block index where current term started
    uint32_t termStartIndex = 0;   // byte offset within doc-area of block
    uint32_t termPostingCount = 0; // total postings for current term
    bool haveOnePosting = false;

    PostingEntry p;
    while (readNextRecord(ifs, p))
    {
        if (!haveOnePosting)
        {
            // first posting ever -> initialize term tracking
            currentTerm = p.term;
            termStartBlock = blockCount;
            termStartIndex = static_cast<uint32_t>(block.docIds.size());
            termPostingCount = 0;
            haveOnePosting = true;
        }

        if (p.term != currentTerm) // does not include first ever posting
        {
            // new term! prev term finished -> write lexicon entry using termStartBlock/termStartIndex/termPostingCount
            LexiconEntry entry{termStartBlock, termStartIndex, termPostingCount};
            writeLexiconEntry(lexicon, currentTerm, entry);

            // reset for new term
            currentTerm = p.term;
            termStartBlock = blockCount;
            termStartIndex = static_cast<uint32_t>(block.docIds.size());
            termPostingCount = 0;
        }

        block.docIds.push_back(p.docId);
        block.freqs.push_back(p.freq);
        ++termPostingCount;

        // flush block if full
        if (block.docIds.size() == MAX_BUF_POSTINGS)
        {
            // write block
            compressBlock(ofs, block, buffer, metadata, blockCount);
            block.clear();
        }
    }

    // final flush
    if (haveOnePosting)
    {
        if (!block.docIds.empty())
        { // still have remaining but not full block
            compressBlock(ofs, block, buffer, metadata, blockCount);
            block.clear();
        }

        // write lexicon for last term
        LexiconEntry entry{termStartBlock, termStartIndex, termPostingCount};
        writeLexiconEntry(lexicon, currentTerm, entry);
    }

    // write metadata
    if (!metadata.empty())
    {
        metadataOut.write(reinterpret_cast<char *>(metadata.data()), metadata.size() * sizeof(BlockMetadata));
    }

    // close files
    ofs.close();
    lexicon.close();
    metadataOut.close();
}

int main()
{
    using namespace std::chrono;
    auto startTime = high_resolution_clock::now();

    generateInvertedIndex();

    auto endTime = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(endTime - startTime).count();
    std::cout << "Elapsed time: " << duration << " ms" << std::endl;
}