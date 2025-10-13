# WebSearchEngines-HW2

## TODOS
- comments for final code once decided
- use llm to return snippets of 10 results - return a vector of up to 10 results and print the doc id with the snippets that are query dependent
- browser-accessible interface
- might be a bug when u have multiple terms in conjunctive - still figuring it out (e.g. terms "archie moore bash ali" return none when 4509151 has all those terms)
- currentPos of listPointer updates when galloping are tricky

## Description
- BM25-Ranked Web Search Engine

## Files & What they Do
1) parsing.cpp (ran for 669708 ms or 11.2 minutes)
- input: MS MARCO dataset
- parses through MS MARCO dataset (input), cleans the input from non-ascii, separating characters (e.g. ; , () --)
- generats intermediate postings of (term, docId, freq)
- quicksorts a buffer of intermediate postings before flushing to disk
- outputs:
    - "page_table.bin": Each index is the length of that docID
    - 256 binary sorted temp files of intermediate postings
        - "temp0.bin"
        - "temp1.bin"
        - ...
        - "temp255.bin"

2) merging.cpp (ran for 1064688 ms or 17.7 minutes)
- input: 256 sorted binary temp files
- merges the 256 input files into 16 binary temp files then into 1 large sorted binary index file uncompressed
- uses heap merge sort / unix sort
- each posting in the final index looks like (termLen, term, docId, freq)
- outputs:
    - 16 temp files (merged from 256)
        - "merging16_0.bin"
        - "merging16_1.bin"
        - ...
        - "merging16_15.bin"
    - 1 temp file (merged from 16)
        - "final_merged.bin"

3) index.cpp (ran for 122097 ms or 2 minutes)
- input: sorted uncompressed index file ("final_merged.bin")
- iterates through each record, then:
    - if the term is the same, consolidate them into blocks/chunks of size 128
    - when encounter a different term, write previous term to lexicon and continue blocking/chunking docIds and frequencies
- each "block" has docIds and frequencies
- inverted index compressed via varbyte compression
- each lexicon entry stores:
    - termSize
    - term
    - start block of the term
    - start index within start block of the term
    - total number of postings for the term
- each metadata entry stores for each block:
    - last docID in that block 
    - compressed size of docIDs for that block
    - compressed size of frequencies for that block
- outputs:
    - lexicon ("lexicon.bin")
    - metadata ("metadata.bin")
    - compressed inverted index ("compressed_inverted_index.bin")

4) query.cpp 
- runtime:
    - for conjunctive on "tinting" and "eyebrow" -> 1653 ms
    - for disjunctive on "difference", "painting", and "dry" -> 2113 ms
- inputs:
    - page table ("page_table.bin")
    - lexicon ("lexicon.bin")
    - metadata ("metadata.bin")
    - compressed inverted index ("compressed_inverted_index.bin")
- ListPointer class for inverted index API framework for openList, closeList, nextGEQ, and getScore
- Implements galloping for nextGEQ
- Decodes docIDs using varbyte
- Implements conjunctive and disjunctive DAAT (maxscore) using top k min heap
- Loads lexicon, page table, and metadata into main memory
- Prefix sums the block offsets at the start