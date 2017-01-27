#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <map>
#include <string>
#include <fstream>
#include <list>
#include <cstring>
#include <limits.h>

#ifndef NOTHREADS
#include <pthread.h>
#include <semaphore.h>
#endif

#include "lookup.h"
#include "str8.h"
#include "trie.h"

// version of strait razor!
const float VERSION_NUM = 3.0;

using namespace std;

#define RECSINMEM 1000000
unsigned LASTREC = UINT_MAX;



// for the command-line options
struct Options {
  unsigned minPrint; // haplotype counts < minPrint will not be printed. default 1
  bool shortCircuit; // can strs be nested? default 0
  bool verbose;// prints out extra information
  bool help; // prints a helpful usage statement. default 0
  bool noReverseComplement; // turns of reverse-complementing markers inferred to be on the negative strand
  FILE *out; // the output file. default=stdout
  char *config; // required! This is the NAME of config file
  int numThreads;
  unsigned char mode; // search style
  unsigned char distance; // hamming or unit edit distance; used when constructing the trie
  bool useTrie; // defunct; always 1
  char *type;// default: NULL can constrain the config file to be just AUTOSOMES (filters on type in the config file)
};


// the anatomy of what we keep for a haplotype
struct Report {
  unsigned strIndex; // which haplotype
  binaryword *haplotype; // unique sequence between the flanks
  int hapLength; // the length of *haplotype in CHARACTERS (ie, in the set {GATC}). ie, the number of bits*2 used in *haplotype
};

struct CompareReport {

  bool operator() (const Report &a, const Report &b) const {
    //TODO: Handle string/float pair instead of lexicographic ordering

    unsigned i1 = a.strIndex;
    unsigned i2 = b.strIndex;

    // different loci
    if (i2 < i1) 
      return false;
    else if (i1 < i2) 
      return true;


    // different haplotype lengths; ergo different haplotypes
    if (a.hapLength < b.hapLength)
      return true;
    else if (a.hapLength > b.hapLength)
      return false;

    // compute the (min) length in binarywords of the two haplotypes
    unsigned i, lengthInWords = ceil( min(a.hapLength, b.hapLength)/ (MAXWORD/2.0));
    binaryword *b1 = a.haplotype;
    binaryword *b2 = b.haplotype;

    for (i=0; i < lengthInWords; ++i, ++b1, ++b2) {
      if (*b1 < *b2)
	return true;
      else if (*b1 > *b2)
	return false;
    }

    return false;
    
  }

};




Options opt; // parsed command-line options

// variables used for IO:
bool done=0; // whether or not the file has been consumed
bool nextdone=0; // this is the penultimate done ; ie, whether or not the 2nd buffer exhausted the filehandle
string MEM[ RECSINMEM ]; // buffers used for processing
string OTHERMEM[ RECSINMEM ]; // these contain the DNA strings from the fastq file
string *records; // pointer used to alterante between MEM and OTHERMEM
istream *currentInputStream; //pointer to stdin / current file opened for reading. fastq format is assumed.



// multithreading variables:
#ifndef NOTHREADS
pthread_mutex_t ioLock;
sem_t writersLock; // the writer is blocked at this array

volatile int workersWorking=0; // the number of workers processing fastq records
volatile bool buffered=0;
#endif



// information about the strs from the file:
vector<Config> *c;
unsigned numStrs; // number of records in the array *c
int minFrag; // the (minimum) fastq record size. inferred from all c
int minLen; // min length of an anchor sequence
int maxLen; // max length of an anchor sequence
Trie *trie=NULL; // is only used with the Trie search...


// the data structures that store the OUTPUT of the computation
// data structure we used to search for matching anchor sequences
vector< Kmer > *vec;
unsigned **biasCounts; // counts the number of times you see a left-hand anchor match, a motif match, and no right-hand anchor ; given for each STR
unsigned **totalCounts; // counts the number of times you see a left-hand anchor match, a motif match, and A right-hand anchor ; given for each STR

unsigned long **leftFlankPosSum; // the position (sum) of the left-hand position match
unsigned long **rightFlankPosSum; // the position (sum) of the right-hand position match

// the data structure used to keep the results
typedef map<Report, pair<unsigned, unsigned>, CompareReport> Matches;
Matches *matches;



// I need to sort reports by both the key, and within equivalent keys, by value.
bool
sortKeyAndValue(pair<Report, pair<unsigned, unsigned> > first, pair<Report, pair<unsigned, unsigned> > second) {
  

      // first sort on the marker name
  unsigned i1 = first.first.strIndex;
  unsigned i2 = second.first.strIndex;

  // different loci
  if (i2 < i1) 
    return false;
  else if (i1 < i2) 
    return true;


  // markers are the same.
  // sort in descending order by their frequency
  if (first.second.first + first.second.second > second.second.first + second.second.second)
    return true;

  return false;
}


void
printReports(FILE *stream, map< Report, pair <unsigned, unsigned>, CompareReport> &hash, unsigned minCount, bool noRC) {


  vector< pair<Report, pair<unsigned, unsigned> > > vec(hash.begin(), hash.end() );
  sort( vec.begin(), vec.end() , sortKeyAndValue);


  vector< pair<Report, pair<unsigned, unsigned> > >::iterator it = vec.begin();

  unsigned i, j, stop, mod, period, offset;
  string s;
  for ( ; it != vec.end(); ++it) {
    if (it->second.first + it->second.second < minCount)
      continue;

    Report rep = it->first;
    s = (*c)[rep.strIndex].locusName;

    // compute the STR nomenclature
    period = (rep.hapLength-(*c)[rep.strIndex].motifOffset)/ (*c)[rep.strIndex].motifPeriod;
    offset = (rep.hapLength-(*c)[rep.strIndex].motifOffset) % (*c)[rep.strIndex].motifPeriod;
    if (offset) {
      fprintf(stream, "%s:%u.%u\t%u bases\t",  s.c_str(), period, offset,
	     rep.hapLength);
    } else {
      fprintf(stream, "%s:%u\t%u bases\t", s.c_str(), period, 
	      rep.hapLength);
    }

    stop = (rep.hapLength / (MAXWORD/2.0));
    mod = rep.hapLength % (MAXWORD/2);
    for (i=0; i < stop; ++i) 
      printBinaryWord(stream, rep.haplotype[i], MAXWORD/2);

    if (mod) 
      printBinaryWord(stream, rep.haplotype[i], mod);

    if (noRC)
      fprintf(stream, "\t\t%u\n", it->second.first + it->second.second );
    else 
      fprintf(stream, "\t%u\t%u\n", it->second.second, it->second.first );


    

  }
  
  if (opt.verbose) {
    fprintf(stream, "\n\nBias Reporting\nMarkerName\tMissingRightanchor_Counts\tTotalMatches_Count\tRatio\tAvgLeftPos\tAvgRightPos\n");
    unsigned sum, sumt;
    unsigned long leftC, rightC;
    for (j=0; j < numStrs; ++j) {
      sumt=sum=0;
      leftC=rightC=0;
      for (i=0; i < (unsigned)opt.numThreads; ++i) {
	sum += biasCounts[i][j];
	sumt += totalCounts[i][j];
	leftC += leftFlankPosSum[i][j];
	rightC += rightFlankPosSum[i][j];
      }

      fprintf(stream,  "%s\t%u\t%u\t" , (*c)[j].locusName.c_str(), sum, sumt);
      if (sumt + sum) {
	printf("%.4f\t", sum/(double)(sumt+sum));
      } else {
	printf("NaN\t");
      }

      if (sumt) {
	fprintf(stream, "%.1f\t%.1f\n", leftC/(double)sumt, rightC/(double)sumt);
      } else {
	fprintf(stream, "NaN\tNaN\n");
      }
      

    }
    

  }

}


// this function is to be called once ALL of the threads have been joined.
// it combines all of the STR matches found across threads, and pools them into matches[0]
// more efficient (multithreaded) design strategies are possible here.
void
printReportsMT(FILE *stream) {
  int i;
  // merge the maps
  for (i=1 ; i < opt.numThreads; ++i) {
    Matches::iterator itr = matches[i].begin();
    // sum up all of the unique haplotypes itr->first
    // with their associated frequency counts
    for( ; itr != matches[i].end(); ++itr) {
      matches[0][ itr->first ].first += itr->second.first;
      matches[0][ itr->first ].second += itr->second.second;
    }
  }
  printReports(stream, matches[0], opt.minPrint,opt.noReverseComplement);
}



bool
buffer(string mem[]) {

  unsigned i=0;
  string dummy;

  while (getline(*currentInputStream, mem[i])) {
    getline(*currentInputStream, mem[i]); // read in the DNA string
    getline(*currentInputStream, dummy); // the +
    getline(*currentInputStream, dummy); // the quality string (ignored)
    ++i;
    if (i == RECSINMEM) {
      int c = currentInputStream->peek();
      if (c == EOF) {
	LASTREC = RECSINMEM;
	return 1;
      }
      return 0;
    }
  }
  LASTREC = i;
  for ( ; i < RECSINMEM; ++i)
    mem[i].clear();

  return 1;
}


/*
  This method takes in:
  dnasequence
  left offset of a haplotype we want
  right offset of a haplotype we want
  the orientation of the match +==FORWARD, -==REVERSE,
  the period of the str
  and the offset of the str
  and the thread id (for thread-safety)

  and it adds the corresponding record to the table of haplotypes

 */
void
makeRecord(const char* dna, unsigned left, unsigned right, unsigned char orientation, unsigned strIndex, int id) {

  //  cout << "HERE!\t"  << left << "\t" << right << endl << dna << endl;

  int len = (int) (right - left);
  int wordlen = ceil(len / (MAXWORD/2.0)); //number of binarywords to represent a haplotype
  binaryword *t, *haplotype = new binaryword[ wordlen ];
  t = haplotype;
  
  for (int i=0; i < len; i += (MAXWORD/2), ++t) {
    *t = gatcToLong((char*)(&dna[i + left ]), MAXWORD/2);
  }
  
  unsigned mod = len % (MAXWORD/2);
  
  if (mod) { // is the sequence not a multple of 32?
    --t;
    *t = *t & ((binaryword)MAXBINARYWORD) << 2*((MAXWORD/2)-mod); // mask out the (right-most) character
  }
  
  if (opt.noReverseComplement== 0 && orientation==REVERSEFLANK) {
    binaryword *hap2 = new binaryword[ wordlen ];
    reverseComplement(haplotype, hap2, wordlen);
    delete(haplotype);
    haplotype=hap2;
  }

  Report rep = {strIndex, haplotype, len};
  
  if (orientation==REVERSEFLANK) {
    ++(matches[id][rep].first);
  } else {
    ++(matches[id][rep].second);
  }
  
  if (opt.verbose) {
    ++totalCounts[id][strIndex];
    leftFlankPosSum[id][strIndex] += left;
    rightFlankPosSum[id][strIndex] += right;
  }


}

// id is the thread ID (set to 1 with no multithreading) matchIds are just indexes in the config, and the type is just the 4 types...
void
processDNA_Trie(int id, unsigned *matchIds, unsigned char *matchTypes) {
  
  int a;
  vector< vector<unsigned> > fpMatches( numStrs, vector<unsigned>(10) ); // forwardflank, positive strand
  vector< vector<unsigned> > rpMatches( numStrs, vector<unsigned>(10) ); // reverse flank, positive strand 
  vector< vector<unsigned> > frMatches( numStrs, vector<unsigned>(10) ); // ditto for negative strand
  vector< vector<unsigned> > rrMatches( numStrs, vector<unsigned>(10) );

  vector<unsigned > lastHitRecord( numStrs, RECSINMEM ); // the index (a in below loop) that yielded a valid hit for a paritcular locus
  vector<unsigned char > validMotif(numStrs, 0); // whether or not there's a valid motif found for a particular STR
  // valid means after a forwardflank or reverseflank_rc (the flanks may not be paired)

  for (a=id ; a < RECSINMEM; a += opt.numThreads) {
    const char *dna = records[a].c_str(); // ascii representation of DNA string
    unsigned dnalen = records[a].length();
    if ((int)dnalen < minFrag)
          continue;
    
    if ((unsigned)a >= LASTREC)
      break;

    bool gotOne=false;
    unsigned stop = dnalen - minLen + 1;
    for (unsigned j=0; j < stop; ++j, ++dna) {
      // this finds demonstrably wrong sequences (less than 'A' or greater than 'T' in the ascii table)
      if (*dna < 65 || *dna > 84) {
	cerr << "Oops! I'm detecting an illegal character in the DNA string. The character is " << dna[j] << endl <<
	  "And the record is " << dna << endl;
	exit(EXIT_FAILURE);
      }


      // if we dont' have a single match (forwward or reverse complement of reverse
      // and there isn't enough sequence to get the smallest possible fragment out, then we're done
      if (! gotOne &&
	  dnalen - j < (unsigned)minFrag)
	break;


      unsigned numMatches = trie->findPrefixMatch((char*)(dna), maxLen, matchIds, matchTypes);

      for (unsigned n = 0; n < numMatches; ++n) {
	unsigned strIndex = matchIds[n];
	unsigned char orientation = matchTypes[n];


#if 0
	if (orientation < MOTIF) 
	  cerr << "Read offset: " << j << " read # " << 
	    a << " Str index " << strIndex << " Orientation " << (unsigned)orientation << endl;
#endif

	if (lastHitRecord[ strIndex ] != (unsigned) a &&
	    orientation < MOTIF) { // clear out the vectors for this STR; it's the first time we've seen this marker (in this read)
	  fpMatches[strIndex].clear();
	  rpMatches[strIndex].clear();
	  frMatches[strIndex].clear();
	  rrMatches[strIndex].clear();
	  validMotif[strIndex]=0; // no valid motifs found
	  lastHitRecord[strIndex ] = a;
	}



	if (orientation == MOTIF || orientation == MOTIF_RC) { // common case
	  if (lastHitRecord[ strIndex ] == (unsigned) a &&  ! validMotif[strIndex]) {  // we have at least one flank found for this locus for this read
	    // motifs! (currently the strand is ignored, as per the previous str8razor

	    // we have at least one (valid) forward flank
	    // and not enough reverse flanks
	    if (fpMatches[strIndex].size() > 0 && 
		rpMatches[strIndex].size() < (*c)[strIndex].reverseCount) {
	      
	      validMotif[strIndex]=1;
	      // match is on the negative strand
	    } else if (rrMatches[strIndex].size() > 0 && 
		     frMatches[strIndex].size() < (*c)[strIndex].forwardCount) {
	      validMotif[strIndex]=1;
	    }

	  }
	// record where the flanks were found, and in which orientation
	} else if (orientation==FORWARDFLANK) {

	  if (! frMatches[strIndex].empty() &&
	      frMatches[strIndex].back()  >= j - (*c)[strIndex].forwardLength) {
	    
	    continue; // overlap!
	  }


	  
	  fpMatches[ strIndex ].push_back(j + (*c)[strIndex].forwardLength); // the index of the first base of the intervening haplotype
	  gotOne=true; 

	} else if (orientation == REVERSEFLANK) {

	  // check to see if this reverse flank that we have is a substring (or overlaps) the forward flank (sadly, this happens. thank you Y chromosome!)
	  // if so, let's skip it
	  if ( ! fpMatches[strIndex].empty() && 
	       fpMatches[strIndex].back()  >= j) {
	    continue;
	  }

	  rpMatches[ strIndex ].push_back(j);
	  // if specified, stop the search when we find the first STR that appears correct
	  if (opt.shortCircuit && 
	      rpMatches[ strIndex ].size() == (*c)[strIndex].reverseCount &&
	      fpMatches[ strIndex ].size() == (*c)[strIndex].forwardCount)
	    break;

	} else if (orientation == FORWARDFLANK_RC) {

	  // and again, but on the negative strand;
	  // the 2nd flank overlaps the first flank; let's assume that the second match in the overlap is wrong.
	  if (! rrMatches[strIndex].empty() &&
	      rrMatches[strIndex].back()  >= j) {
	    continue;
	  }

	  frMatches[ strIndex ].push_back(j);
	  if (opt.shortCircuit && 
	      rrMatches[ strIndex ].size() == (*c)[strIndex].reverseCount &&
	      frMatches[ strIndex ].size() == (*c)[strIndex].forwardCount)
	    break;


	} else if (orientation == REVERSEFLANK_RC) {


	  if (! frMatches[strIndex].empty() &&
	      frMatches[strIndex].back()  >= j - (*c)[strIndex].reverseLength) {
	    
	    continue; // overlap!
	  }


	  rrMatches[ strIndex ].push_back(j + (*c)[strIndex].reverseLength); // the index of the first base of the intervening haplotype
	  gotOne=true; 

	  // haven't found a valid motif for this STR yet
	} 

      }
    } // done reading read
    dna = records[a].c_str(); // reset the pointer
    
    if (gotOne) { // is there at least one record
      for (unsigned i = 0; i < numStrs; ++i) {
	if (lastHitRecord[i]== (unsigned) a 
	    && validMotif[i]
	    && (fpMatches[i].size() == 0 || rrMatches[i].size()==0) // ensure that the STR is only appearing once, and once in the proper orientation
	    ) { // this STR was found for this read
	  // positive strand match
	  if (fpMatches[i].size() == (*c)[i].forwardCount) {
	    if (rpMatches[i].size() == (*c)[i].reverseCount ) {
	      
#if DEBUG
	      cerr << c->at(i).locusName << " STR on forward strand " << i << " fpsize " << fpMatches[i].size() <<
		" rpsize " << rpMatches[i].size() << endl;
#endif
	      
	      if (fpMatches[i].back()  < rpMatches[i].front())
		makeRecord(dna, fpMatches[i].front(), rpMatches[i].back(), FORWARDFLANK, i, id);

	    } else if (opt.verbose && rpMatches[i].size() < (*c)[i].reverseCount ) { // not enough matches for the second anchor
	      ++biasCounts[id][i];
	    }
	    // negative strand match
	  } 
	  if ( rrMatches[i].size() == (*c)[i].reverseCount) {
	    if ( frMatches[i].size() == (*c)[i].forwardCount ) {
	      
#if DEBUG
	      cerr << c->at(i).locusName << " STR on reverse strand " << i << " fpsize " << fpMatches[i].size() <<
		" rpsize " << rpMatches[i].size() << endl;
#endif
	      
	      if (rrMatches[i].back() < frMatches[i].front() )
		makeRecord(dna, rrMatches[i].front(), frMatches[i].back(), REVERSEFLANK, i, id);
	      
	    } else if (opt.verbose && frMatches[i].size() < (*c)[i].forwardCount ) {
	      ++biasCounts[id][i]; 
	    }
	    
	  }
	}
      }//for
    } // gotone
  }
  
  
}





void
findMatchesOneThread() {

  bool done=false;

    // a ceiling on the number of matches you can find on a single pass through the trie
  unsigned *matchIds = new unsigned[ numStrs * 4];
  unsigned char *matchTypes = new unsigned char[ numStrs * 4];
  
  records = MEM; // we're only using one of the buffers...
  while (!done) {
    done = buffer(MEM);
    processDNA_Trie(0, matchIds, matchTypes);
  }
    
  delete [] matchIds;
  delete [] matchTypes;

  printReports(opt.out, matches[0], opt.minPrint,opt.noReverseComplement);
}



void
usage(char *arg0) {

  cerr << "Correct usage for version Strait Razor v" << VERSION_NUM << endl << arg0 << " -c configFile [OPTIONS] fastqfile1 [fastqfile2 ... ]" << endl << "OR" << endl << arg0 << " -c configFile [OPTIONS] < fastqfile1" << endl;
  cerr << endl << "IE, This program takes in standard input, or a bunch of (uncompressed) fastq files\nAnd remember, options are specified *before* the configfile and fastqs (ie, the arguments)" << endl << endl;
  cerr << "Possible arguments:" << endl << endl << 
    "\t-h (help; causes this to be printed)" << endl <<
    "\t-n (no reverse complement-- this turns off the default behavior of reverse-complementing matches on the negative strand)" << endl <<
    "\t-v (verbose ; prints out additional diagnostic information)" << endl << endl <<

    "\t-d distance (default 1; the maximum Hamming distance used with anchor search. can only be 0, 1 or 2)" << endl <<
    "\t-c configFile (REQUIRED; the locus config file used to define the STRs)" << endl << 
    "\t-p integer (The number of processors/cpus used)" << endl <<
    "\t-t filter (This filters on Type, e.g. AUTOSOMES; ie, it restricts the output to STRs that have the same type as specified in column 2 of the config file)" << endl <<
    "\t-o filename (This writes the output to filename, as opposed to standard out)" << endl <<
    "\t-m integer (Min match; this causes haplotypes with less than m occurences to be omitted from the final output file" << endl << endl;
  exit(EXIT_FAILURE);
}


// used to parse command line arguments
// returns the index in the argv that (may or may not) contain fastq files
int
parseArgs(int argc, char **argv, Options &opt) {
  
  int i;

  // defaults
  opt.out = stdout;
  opt.verbose=0;
  opt.help=0;
  opt.shortCircuit=0;
  opt.minPrint=0;
  opt.noReverseComplement=0; 
  opt.config=NULL;
  opt.numThreads=1;
  opt.useTrie=1;
  opt.type=NULL;
  opt.distance=1;

  bool errors=0;
  
  for (i=1; i < argc; ++i) {
    if (argv[i][0] == '-') {
      if (argv[i][1] == 'h') {
	opt.help=1;
      } else if (argv[i][1] == 's') {
	opt.shortCircuit=1;
      } else if (argv[i][1] == 'v') {
	opt.verbose=1;
      } else if (argv[i][1] == 't') {
	// filters the config file by type
	++i;
	opt.type = argv[i];
	if (opt.type==NULL) {
	  cerr << "Oops. -t flag requires a type!" << endl;
	  ++errors;
	}
      } else if (argv[i][1] == 'n') {
	opt.noReverseComplement=1; 
      } else if (argv[i][1] == 'o') { // setting output file
	if (i == argc-1) {
	  cerr << endl<< "Option -o requires an argument\nEG, -o outFile.txt" << endl << endl;
	  errors=1;
	} else {
	  ++i;
	  opt.out = fopen(argv[i], "w");
	}
      } else if (argv[i][1] == 'm') { // setting min records to print
	if (i == argc-1) {
	  cerr << endl << "Option -m requires an integer; the smallest number of haplotype-counts that are allowed to be printed" << endl << endl;
	  errors=1;
	} else {
	  ++i;
	  char *s = argv[i];
	  while (*s >= '0' && *s <= '9') {
	    opt.minPrint = (opt.minPrint*10)+(*s - '0');
	    ++s;
	  }
	  if (s == argv[i]) {
	    cerr << endl << "Option -m requires an integer; the smallest number of haplotype-counts that are allowed to be printed, not " << s  << endl << endl;
	    errors=1;
	  }
	}
      } else if (argv[i][1] == 'd') { // setting the degeneracy/distance used when constructing the trie
	if (i == argc-1) {
	  cerr << endl << "Option -d requires an integer in the range [0,2] (inclusive); the (max) edit/hamming distance used when searching for anchors" << endl << endl;
	  errors=1;
	} else {
	  ++i;
	  char *s = argv[i];
	  opt.distance = (*s - '0');
	  if (s[1] != 0 || opt.distance > 2) {
	    cerr << endl << "Option -d requires a an integer in the range [0,2] (inclusive); the (max) hamming distance used when searching for anchors, not: " << s << endl;
	    errors=1;
	  }
	  //	  cerr << "Distance is " << ((unsigned)opt.distance) << endl;
	}
      } else if (argv[i][1] == 'p') { // setting min records to print
	if (i == argc-1) {
	  cerr << endl << "Option -p requires an integer; the number of processors needed" << endl << endl;
	  errors=1;
	} else {
	  ++i;
	  char *s = argv[i];
	  opt.numThreads=0;
	  while (*s >= '0' && *s <= '9') {
	    opt.numThreads = (opt.numThreads*10)+(*s - '0');
	    ++s;
	  }
	  if (s == argv[i]) {
	    cerr << endl << "Option -p requires an integer; not " << s  << endl << endl;
	    errors=1;
	  }

#ifdef NOTHREADS

	  if (opt.numThreads > 1) {
	  cerr << "This program was compiled w/o threads, yet you are trying to use multithreading. You can't do both!" << endl <<
	    "Please either recompile this program with threads, or set the number of threads to 1!!" << endl;
  	    exit(EXIT_FAILURE);
	  } 
#endif

	}
      } else if (argv[i][1] == 'c') { // this config file
	if (i == argc-1) {
	  cerr << endl<< "Option -c requires a file; ie, the config file" << endl << endl;
	  errors=1;
	} else {
	  ++i;
	  opt.config = argv[i];
	}
      } else if (argv[i][1] == '-') { // unix convention; force the end of flags
	break;
      } else {
	cerr << endl <<  "Unknown option: " << argv[i]  << endl << endl;
	errors=1;
      }

    } else
      break;

  }

  if (opt.config==NULL) {
    cerr << endl <<  "Missing required flag: -c configfile"  << endl << endl;
    errors=1;
  }

  if (errors) 
    usage(argv[0]);

  return i;
}

#ifndef NOTHREADS

void *
workerThread(void *arg) {
  
  int id =  *((int*)arg ); // thread id
  
  unsigned *matchIds = NULL;
  unsigned char *matchTypes = NULL;
  matchIds = new unsigned[ numStrs * 4];
  matchTypes = new unsigned char[ numStrs * 4];


 top:
  // spin loop... wait for the producer to generate some data
  while (workersWorking < opt.numThreads) 
    ;

 middle:

  processDNA_Trie(id, matchIds, matchTypes);

  if (done) {

    if (opt.useTrie) {
      delete [] matchIds;
      delete [] matchTypes;
    }

    return NULL;
  }
  pthread_mutex_lock(&ioLock);

  --workersWorking;

  // last worker to check in
  if (workersWorking == 0) {    

    while (! buffered) // ensure that the other buffer is filled... 
      ; // ie, wait for buffered=1 assignment

    if (records==MEM)
      records = OTHERMEM;
    else
      records = MEM;

    buffered=0; // we set buffered=0; ie, the other buffer needs to get filled
    done = nextdone; // update done with the status of the current buffer
    sem_post(&writersLock); // wake up the writer thread which fills the buffer
    

    workersWorking= opt.numThreads; // wake up the othe readers
    pthread_mutex_unlock(&ioLock);  // release mutex
    goto middle;
  } else {
    pthread_mutex_unlock(&ioLock);
    goto top;
  }

}


void *
writerThread(void *arg) {

  nextdone = done=buffer(MEM); // read in a bunch of data
  records = MEM; // set up the pointer
  workersWorking= opt.numThreads; // let the workers start processing the data

  while (! nextdone) {

    if (records == MEM) 
      nextdone=buffer(OTHERMEM);
    else 
      nextdone=buffer(MEM);

    buffered=1; // the double buffer is set up 
    sem_wait(&writersLock); // wait for the readers to finish reading *records

    
  }

  return NULL;

}

#endif


int
main(int argc, char **argv) {

  if (argc < 3) {
    fprintf(stderr, "Not enough arguments!\n");
    usage(argv[0]);
  }

  unsigned i;
  int start = parseArgs(argc, argv, opt);

  if (opt.numThreads == 0)
    opt.numThreads=1;
  
  
  int *ids;
  
  ids = new int [ opt.numThreads];
  matches = new Matches[ opt.numThreads ]; // each thread records the records it found

  
  
  for (i=0; i < (unsigned) opt.numThreads; ++i)
    ids[i] = i;
  
  std::ios::sync_with_stdio(false);
  
  // parse the config file
  c = parseConfig(opt.config, &numStrs, opt.type);

  biasCounts = new unsigned* [ opt.numThreads]; // and counts for partial allelic dropout  
  totalCounts = new unsigned* [ opt.numThreads]; // and counts for partial allelic dropout  

  leftFlankPosSum = new unsigned long* [ opt.numThreads]; // and counts for partial allelic dropout  
  rightFlankPosSum = new unsigned long* [ opt.numThreads]; // and counts for partial allelic dropout  

  for (i=0; i < (unsigned) opt.numThreads; ++i) {
    biasCounts[i] = new unsigned[ numStrs ](); // () initializes the bias counts to 0
    totalCounts[i] = new unsigned[ numStrs ](); // () initializes the total counts to 0

    leftFlankPosSum[i] = new unsigned long [ numStrs ](); 
    rightFlankPosSum[i] = new unsigned long [ numStrs ](); 
  }
  

  if (numStrs==0) {
    fprintf(stderr, "No markers found in file: %s\n", opt.config);
    return 1;
  }


  // compute minima and maxima for the STRs we're looking for
  minLen = min((*c)[0].forwardLength,   (*c)[0].reverseLength);
  maxLen = max(  (*c)[0].forwardLength,   (*c)[0].reverseLength);
  // assumes that the motif nomenclature cannot be negative
  minFrag =   (*c)[0].forwardLength +   (*c)[0].reverseLength + (*c)[0].motifOffset;
  for (i=1; i < numStrs; ++i) {

    if ((int)  (*c)[i].forwardLength < minLen)
      minLen =   (*c)[i].forwardLength;

    if ((int)  (*c)[i].reverseLength < minLen)
      minLen =   (*c)[i].reverseLength;

    if ((int)  (*c)[i].forwardLength > maxLen)
      maxLen =   (*c)[i].forwardLength;

    if ((int)  (*c)[i].reverseLength > maxLen)
      maxLen =   (*c)[i].reverseLength;


    int fragLen =   (*c)[i].forwardLength +   (*c)[i].reverseLength + (*c)[i].motifOffset;
    if (fragLen < minFrag)
      minFrag = fragLen;
  }



  trie = new Trie;
  trie->makeTrieFromConfig(c, numStrs, opt.distance);


#ifndef NOTHREADS
  if (opt.numThreads > 1) {
    pthread_mutex_init(&ioLock, 0);
    sem_init(&writersLock, 0, 0);
  }
#endif

  
  for (i=start ; i < (unsigned)argc; ++i) {
    ifstream in;
    in.open(argv[i], ios::in);
    if (! in.is_open() ) {
      cerr << "Failed to open " << argv[i] << " for reading\n";
      continue;
    }
    currentInputStream = &in;


    if (opt.numThreads < 2) 
      findMatchesOneThread( );
    else {
#ifndef NOTHREADS
      int err;
      pthread_t t;
      list <pthread_t> threads;
            
      err = pthread_create(&t, NULL, writerThread, NULL ); 
      if (err) {
        cerr << "Error creating writer thread" << endl;
	exit(EXIT_FAILURE);
      }
      threads.push_back(t);

      for (int j=0; j < opt.numThreads; ++j) {
	err = pthread_create(&t, NULL, workerThread, (void*) &(ids[j]) ); 
	if (err) {
	  cerr << "Error creating thread number: " << j << endl;
	  exit(EXIT_FAILURE);
	}
	threads.push_back(t);
      }
      for (list<pthread_t>::iterator itr = threads.begin(); itr != threads.end(); ++itr) { // collect the threads when we're done
	t = *itr;
	pthread_join(t, NULL);
      }

      threads.clear();
      std::ios::sync_with_stdio(true);
      printReportsMT(opt.out);
      std::ios::sync_with_stdio(false);
#endif

    }
    in.close();
  }


  // if no fastq files are given then check stdin
  if (argc == start)  {
    currentInputStream = &cin;
    if (opt.numThreads < 2) 
      findMatchesOneThread( );

    else {
#ifndef NOTHREADS
      int err;
      pthread_t t;
      list <pthread_t> threads;
            
      err = pthread_create(&t, NULL, writerThread, NULL ); 
      if (err) {
        cerr << "Error creating writer thread" << endl;
	exit(EXIT_FAILURE);
      }
      threads.push_back(t);
      
      for (int j=0; j < opt.numThreads; ++j) {
	err = pthread_create(&t, NULL, workerThread, (void*) &ids[j] ); 
	if (err) {
	  cerr << "Error creating thread number: " << j << endl;
	  exit(EXIT_FAILURE);
	}
	threads.push_back(t);
      }
      for (list<pthread_t>::iterator itr = threads.begin(); itr != threads.end(); ++itr) { // collect the threads when we're done
	t = *itr;
	pthread_join(t, NULL);
      }

      threads.clear();
      std::ios::sync_with_stdio(true);
      printReportsMT(opt.out);
      std::ios::sync_with_stdio(false);
#endif

    }

  }


#ifndef NOTHREADS
  if (opt.numThreads > 1) {
    pthread_mutex_destroy(&ioLock);
    sem_destroy(&writersLock);
  }
#endif

  // let's close our file handles, shall we?
  fclose(opt.out);

  // optionally can free memory here...
  // delete ids;

  return 0;
}



  
