#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <vector>
#include <queue>
#include <chrono>
using namespace std;

const size_t BUF_SIZE = 100 * 1024 * 1024; // 100 MB
const int FILE_COUNT = 256;
const int GROUP_SIZE = 16;

struct PostingEntry
{
    string term;
    int docId;
    int freq;
    int fileIndex; // original temp file to identify replacement for heap
};

// functor comparator for min-heap
struct ComparePosting
{
    bool operator()(const PostingEntry &a, const PostingEntry &b) const
    {
        if (a.term != b.term)
        {
            return a.term > b.term;
        }
        else
        {
            return a.docId > b.docId;
        }
    }
};

// GLOBALS
vector<ifstream> inputFiles;
vector<char> outputBuf;
size_t outCurPos = 0;

// for min heap, need a greater than comparator, comparator has to be a type
priority_queue<PostingEntry, vector<PostingEntry>, ComparePosting> minHeap;

// initially had used custom buffers for ifstreams but too complex and bug-prone since full records might not fit in buffer since each record is different sizes, so just streamed directly
bool readNextRecord(int fileIndex, PostingEntry &p)
{
    ifstream &ifs = inputFiles[fileIndex];
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

    p = PostingEntry{term, docId, freq, fileIndex};
    return true;
}

void writeMergedRecord(ofstream &ofs, const PostingEntry &p)
{
    uint32_t termLen = static_cast<uint32_t>(p.term.size());
    size_t recordSize = sizeof(termLen) + p.term.size() + 2 * sizeof(int);

    // flush outputBuffer to disk first if too full and can't fit full record
    if (outCurPos + recordSize > outputBuf.size())
    {
        ofs.write(outputBuf.data(), outCurPos);
        outCurPos = 0;
    }

    // copy data into buffer
    char *ptr = outputBuf.data() + outCurPos;
    memcpy(ptr, &termLen, sizeof(termLen));
    ptr += sizeof(termLen);
    memcpy(ptr, p.term.data(), termLen);
    ptr += termLen;
    memcpy(ptr, &p.docId, sizeof(p.docId));
    ptr += sizeof(p.docId);
    memcpy(ptr, &p.freq, sizeof(p.freq));
    ptr += sizeof(p.freq);

    // update next read pos for output buffer
    outCurPos += recordSize;
}

void mergeBuffers(const vector<string> &filenames, const string &outFile)
{
    inputFiles.clear();
    for (const string &filename : filenames)
    {
        ifstream ifs(filename, ios::binary);
        if (!ifs)
        {
            cerr << "Failed to open " << filename << endl;
            exit(1);
        }
        inputFiles.push_back(std::move(ifs));
    }

    ofstream ofs(outFile, ios::binary);
    outputBuf.resize(BUF_SIZE); // 1MB buffer
    outCurPos = 0;

    // initialize heap
    minHeap = {}; // clear heap
    for (size_t i = 0; i < inputFiles.size(); ++i)
    {
        PostingEntry p;
        if (readNextRecord(i, p))
        {
            minHeap.push(p);
        }
    }

    // merge
    while (!minHeap.empty())
    {
        PostingEntry top = minHeap.top();
        minHeap.pop(); // doesn't return the element, just pops
        writeMergedRecord(ofs, top);

        PostingEntry nextToFill;
        if (readNextRecord(top.fileIndex, nextToFill))
        {
            minHeap.push(nextToFill);
        }
    }

    // flush out buffer to disk if leftover
    if (outCurPos > 0)
    {
        ofs.write(outputBuf.data(), outCurPos);
    }

    ofs.close();

    // close input files
    for (ifstream &ifs : inputFiles)
    {
        ifs.close();
    }
}

// group merge (256 to 16)
vector<string> mergeGroups(const vector<string> &filenames, const string &prefix)
{
    vector<string> outputs;
    for (int g = 0; g < GROUP_SIZE; ++g)
    {
        int start = g * GROUP_SIZE;
        int end = min(start + GROUP_SIZE, (int)filenames.size());
        vector<string> subset(filenames.begin() + start, filenames.begin() + end);
        string outName = prefix + to_string(g) + ".bin";
        mergeBuffers(subset, outName);
        outputs.push_back(outName);
    }
    return outputs;
}

int main()
{
    using namespace std::chrono;
    auto startTime = high_resolution_clock::now(); // record start

    // set up 256 temp files
    vector<string> temp256Files;
    for (int i = 0; i < FILE_COUNT; ++i)
    {
        temp256Files.push_back("temp" + to_string(i) + ".bin");
    }

    // merge 256 -> 16
    vector<string> temp16Files = mergeGroups(temp256Files, "merged16_");

    // merge 16 -> 1
    string finalIndex = "final_merged.bin";
    mergeBuffers(temp16Files, finalIndex);

    auto endTime = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(endTime - startTime).count();
    std::cout << "Elapsed time: " << duration << " ms" << std::endl;

    return 0;
}