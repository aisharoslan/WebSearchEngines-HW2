#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <sstream>
#include <cctype>
#include <chrono>
using namespace std;

// intermediate posting
struct Posting
{
    const char *termPtr; // ptr to term in shared term buffer, to ensure same size across all postings
    int docId;
};

const int DATASET_SIZE = 8841823;
const int TEMP_FILES_NUM = 255; // +1 for leftover buffer at the end
const int DOCS_PER_FILE = DATASET_SIZE / TEMP_FILES_NUM;

// for posting buffer
const int POSTING_BUFFER_SIZE = (100 * 1024 * 1024) / sizeof(Posting);
Posting postingBuffer[POSTING_BUFFER_SIZE];
unsigned postingBufferIndex = 0;

// for term buffer
const int TERM_BUFFER_SIZE = 150 * 1024 * 1024; // 150 MB
char termBuffer[TERM_BUFFER_SIZE];              // e.g. [dog\0cat\0apple\0]
unsigned termBufferOffset = 0;                  // current write offset

// for page table
int pageTable[DATASET_SIZE] = {0};

unsigned tempFileCount = 0;
int docCount = 0;

void openFile(ifstream &ifs, const string &inputFile)
{
    ifs.open(inputFile);

    if (!ifs)
    {
        cerr << "Failed to open file: " << inputFile;
        exit(1);
    }
}

bool compareCstring(const Posting &a, const Posting &b)
{
    // <0 if s1 comes before s2
    // =0 if s1 == s2
    // >0 if s1 comes after s2
    int result = strcmp(a.termPtr, b.termPtr);
    if (result == 0)
    {
        return a.docId < b.docId;
    }
    else
    {
        return result < 0;
    }
}

int partition(int low, int high)
{
    Posting pivot = postingBuffer[high];
    int i = low - 1;
    for (int j = low; j <= high - 1; j++)
    {
        if (compareCstring(postingBuffer[j], pivot))
        {
            i++;
            swap(postingBuffer[i], postingBuffer[j]);
        }
    }

    swap(postingBuffer[i + 1], postingBuffer[high]);
    return i + 1;
}

void quickSortBuffer(int low, int high)
{
    if (low < high)
    {
        int partitionIndex = partition(low, high);

        quickSortBuffer(low, partitionIndex - 1);
        quickSortBuffer(partitionIndex + 1, high);
    }
}

int tokenizeSentence(int docId, const string &sentence)
{
    int termCount = 0;
    string term;
    stringstream ss(sentence);
    while (ss >> term)
    {
        if (term.empty())
            continue;

        size_t termLen = term.size() + 1; // include null terminator

        // add to term buffer, increment termBufferOffset by termLen
        char *ptr = termBuffer + termBufferOffset;
        // memcpy(dest, src, n) - copies n bytes from src to dst
        memcpy(ptr, term.c_str(), termLen);
        termBufferOffset += termLen;

        // add to postingBuffer, increment postingBufferIndex
        postingBuffer[postingBufferIndex++] = Posting{ptr, docId};
        ++termCount;
    }
    return termCount;
}

void outputFile()
{
    if (postingBufferIndex == 0)
    {
        return; // nothing to flush
    }

    quickSortBuffer(0, postingBufferIndex - 1);

    const char *lastTermPtr = postingBuffer[0].termPtr;
    int lastDoc = postingBuffer[0].docId;
    int freq = 1;

    // set up file for flush to disk
    string filename = "temp";
    string extension = "bin";
    stringstream ss;
    ss << filename << tempFileCount++ << "." << extension;
    filename = ss.str();
    ofstream ofs(filename, ios::binary);

    // postingBufferIndex is where we last ended
    for (int i = 0; i < postingBufferIndex; ++i)
    {
        const char *termPtr = postingBuffer[i].termPtr;
        int docId = postingBuffer[i].docId;

        if ((strcmp(termPtr, lastTermPtr) == 0) && (docId == lastDoc))
        {
            ++freq;
        }
        else
        {
            // binary
            uint32_t termLen = strlen(lastTermPtr);
            ofs.write(reinterpret_cast<char *>(&termLen), sizeof(termLen));
            ofs.write(lastTermPtr, termLen);
            ofs.write(reinterpret_cast<char *>(&lastDoc), sizeof(int));
            ofs.write(reinterpret_cast<char *>(&freq), sizeof(int));

            lastTermPtr = termPtr;
            lastDoc = docId;
            freq = 1;
        }
    }

    // final term
    uint32_t termLen = strlen(lastTermPtr);
    ofs.write(reinterpret_cast<char *>(&termLen), sizeof(termLen));
    ofs.write(lastTermPtr, termLen);
    ofs.write(reinterpret_cast<char *>(&lastDoc), sizeof(int));
    ofs.write(reinterpret_cast<char *>(&freq), sizeof(int));
    ofs.close();

    // reset for next temp file
    postingBufferIndex = 0;
    termBufferOffset = 0;
}

void cleanSentence(string &sentence)
{
    string cleaned;
    for (char c : sentence)
    {
        unsigned char uc = tolower((unsigned char)c);
        if (uc <= 127 && !ispunct(uc))
        {
            cleaned += uc; // valid non-punctuation ascii
        }
        else
        {
            cleaned += ' '; // replace separating characters with a space to split terms
        }
    }
    sentence = cleaned;
}

void readFile(ifstream &ifs)
{
    int docId;
    string line;
    string sentence;
    while (getline(ifs, line))
    {
        stringstream ss(line);
        ss >> docId;
        getline(ss, sentence);
        cleanSentence(sentence); // clean utf-8 misencodings
        int docLength = tokenizeSentence(docId, sentence);
        pageTable[docId] = docLength;

        ++docCount;
        if (docCount > 0 && (docCount % DOCS_PER_FILE == 0))
        {
            outputFile(); // flush every 34K docs
        }
    }
}

void outputPageTable()
{
    ofstream ofs("page_table.bin", ios::binary);
    ofs.write(reinterpret_cast<char *>(pageTable), sizeof(pageTable));
    ofs.close();
}

int main()
{
    using namespace std::chrono;
    auto startTime = high_resolution_clock::now();

    ifstream ifs;
    string inputFile = "collection.tsv";
    openFile(ifs, inputFile);
    readFile(ifs);
    if (postingBufferIndex > 0)
    {
        outputFile();
    }
    outputPageTable();
    ifs.close();

    auto endTime = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(endTime - startTime).count();
    std::cout << "Elapsed time: " << duration << " ms" << std::endl;

    return 0;
}
