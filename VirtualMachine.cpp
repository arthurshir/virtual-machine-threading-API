//
//  VirtualMachine.cpp
//
//
//  Arthur Shir
//  Priscilla Yu
//  5/3/15
//
//  Notes:
//    - In threadActivate: Might want to resumeMachine signals before running Schedule()
//    - In schedule(): Might want to add suspend/ resume machine signals
//    - waitList is removed: Might want to consider bringing back?
//    - skeletonEntry: runs currenThread instead of activeThread :/
//    -- Added suspension to: threadTerminate,
//    - Pushed a second idle thread to deal with case in VMStart: current = idle, no items in qeueu
//    - Added contextBeforeAlarm to try to return to idle after interrupts

//  Notes P3:
//    - Line 268 Shared Memory Pool

#include "VirtualMachine.h"
#include "Machine.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <vector>
#include <list>
#include <math.h>
#define debug() { cerr << "\n  Debug (" << debugIndex << "."<< debugCounter++ << ")" << endl;}
#define resetDebug() {debugCounter = 1; debugIndex++;}
#define setZeroDebug() {debugCounter = 0; debugIndex++;}
#define rootDirName "ROOT-------"
using namespace std;

extern "C" {
  
  // Prototypes
  void fileCallback(void *calldata, int result);
  uint16_t EndiantoInt( int numBytes, uint8_t* base);
  void ReadSector (int sectorInt);
  void ReadBPB();
  void CheckBPB();
  void PrintCharFat();
  void PrintNumFat();
  void loadAllRootEntries();
  void loadFatTable();
  void ReadCluster(int clusterInt);

  struct TCB{
    TVMThreadEntry entry;
    void *param;
    uint8_t *stackptr;
    TVMMemorySize memsize;
    TVMThreadPriority priority;
    TVMThreadID threadId;
    TVMThreadState state;
    SMachineContext context;
    int ticks = 0;
    int fileResult = -1;
  };
  
  struct mutexHolder{
    int lock;
    int threadID;
  };

  struct block{
    int baseIndex;   // base of free chunk
    int length;      // length of free chunk's Memory Array
  };
  
  struct memPool{
    TVMMemorySize memoryPoolSize;   // Pool Size
    TVMMemoryPoolID memoryPoolID;   // Pool ID
    uint8_t *base;                  // Pointer to base of memory array
    int length;                     // Size of memory array
    int freeSpace;                  // Amount of bytes that are free
    block freeBlock;        // List of blocks of free chunks
    list <block*> allocatedList;   // List of blocks of allocated chunks
  };
  
  struct BPB{
    uint16_t BS_jmpBoot;
    uint8_t* BS_OEMName;
    uint16_t BPB_BytesPerSec;
    uint16_t BPB_SecPerClus;
    uint16_t BPB_RsvdSecCnt;
    uint16_t BPB_NumFATs;
    uint16_t BPB_RootEntCnt;
    uint16_t BPB_TotSec16;
    uint16_t BPB_Media;
    uint16_t BPB_FATSz16;
    uint16_t BPB_SecPerTrk;
    uint16_t BPB_NumHeads;
    uint16_t BPB_HiddSec;
    uint16_t BPB_TotSec32;
    uint16_t BS_DrvNum;
    uint16_t BS_Reserved1;
    uint16_t BS_BootSig;
    uint16_t BS_VolID;
    uint8_t* BS_VolLab;
    uint8_t* BS_FilSysType;
    
    uint16_t FirstRootSector;
    uint16_t RootDirectorySectors;
    uint16_t FirstDataSector;
    uint16_t ClusterCount;
  };
  
  struct rootEntry{
    uint8_t* DIR_Name;
    uint16_t DIR_Attr;
    uint16_t DIR_NTRes;
    uint16_t DIR_CrtTimeTenth;
    uint16_t DIR_CrtTime;
    uint16_t DIR_CrtDate;
    uint16_t DIR_LstAccDate;
    uint16_t DIR_FstClusHI;
    uint16_t DIR_WrtTime;
    uint16_t DIR_WrtDate;
    uint16_t DIR_FstClusLO;
    uint16_t DIR_FileSize;
    
    SVMDirectoryEntry SVMDirEntry;
  };
  
  struct cluster{
    int dirtyBit;
    int nextCluster;
    bool hasNextCluster;
    int startingSector;
    //vector <uint8_t> sectors; 
  };
  
  struct fatEntry{
    vector<cluster*> clusterChain;
  };
  
  // Global Variables
  //memPool *systemMemPool;         // Memory Pool of the System.
  int debugCounter = 1;
  int debugIndex = 1;
  vector <memPool*> allMemPools;
  const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 0;
  const TVMMemoryPoolID VM_MEMORY_POOL_ID_SHARED = 1;
  //uint8_t* sharedBasePointer;
  TVMMainEntry VMLoadModule(const char *module); // Prototype for VMLoadModule
  list <TCB*> lowReadyQueue, normalReadyQueue, highReadyQueue, sleepQueue, waitList;
  vector <TCB*> allOtherThreads;
  TCB* currentThread = new TCB;
  TCB* idleThread = new TCB;
  TCB* vmMainThread = new TCB;
  SMachineContext *contextBeforeAlarm;
  vector <mutexHolder> allMutexes;
  // ---------------- Project 4------------------- //
  uint8_t* fatData;
  BPB *globalBPB;
  vector <rootEntry*> rootTable;
  vector <fatEntry*> fatTable;
  
  uint8_t currentDirectory[11];
  vector <uint8_t[11]> previousDirectories;
  
  int dirReadCounter = 0;
  int currentReadCluster = -1;
  int mountFd;
  int secInClustCounter = 1;
  
  
  // Schedule()
  // Behavior depends on the following cases
  //   1. Same Thread       : just pop front, push to back
  //   2. Waiting           : push to waitList, contextSwitch to next in queue
  //   3. Dead              : contextSwitch to next in queue
  //   4. Running or Ready  : push back into queue, contextSwitch to next in queue
  void Schedule()
  {
    //cerr << "  In Schedule" << endl;
    int idle = 0;
    // Step 1: Check which queue next. If no queue found, this means you must run idle again.
    list <TCB*> *theQueue;
    if (!highReadyQueue.empty()) {
      //cerr << "  High" << endl;
      theQueue = &highReadyQueue;
    }
    else if (!normalReadyQueue.empty()){
      //cerr << "  normal" << endl;
      theQueue = &normalReadyQueue;
    }
    else if (!lowReadyQueue.empty()) {
      //cerr << "  low " << endl;
      theQueue = &lowReadyQueue;
    }
    else 
    {
      idle = 1;
      //cerr << "  Idling " << endl;
      // Load new idle context
      SMachineContext *previousThreadContext = &currentThread->context; // Save old context
      currentThread = idleThread;
      currentThread->state = VM_THREAD_STATE_RUNNING;
      MachineContextSwitch( previousThreadContext, &currentThread->context);
    }
    //else cerr << "  Error, all queues empty... this should not happen" << endl;
    
    // Step 2: Different cases depending upon the state of the Current Thread
    // 1. Same Thread
    // 2. Waiting or Dead
    // 4. Running or Ready
    
    // Current Thread is same as front of Queue
    //   - No change to currentThread
    //   - Reset its position in the Queue
    if (!idle)
    {
      if (currentThread == theQueue->front())
      {
        //cerr << "  Same Thread" << endl;
        TCB *temp = theQueue->front();
        theQueue->pop_front();
        theQueue->push_back(temp);
      }
      else
      {
        // Current Thread state == Waiting or Dead
        //   - Do nothing for current Thread
        //   - Context switch into next thread in readyQueue
        if (currentThread->state == VM_THREAD_STATE_WAITING || currentThread->state == VM_THREAD_STATE_DEAD)
        {
          //cerr << "  Waiting or Dead" << endl;
          waitList.push_back(currentThread);
          //if (currentThread == idleThread) cerr << "  IdleThread: ";
          //if (currentThread->state == VM_THREAD_STATE_WAITING) cerr << "  Wait" << endl;
          //if (currentThread->state == VM_THREAD_STATE_DEAD) cerr << "  Dead" << endl;
          TCB *oldThread = currentThread;
          currentThread = theQueue->front();
          theQueue->pop_front();
          currentThread->state = VM_THREAD_STATE_RUNNING;
          //if (currentThread == idleThread) cerr << "  IdleThread: ";
          MachineContextSwitch( &oldThread->context, &currentThread->context );
        }
        
        // Current Thread state == Running OR Ready
        //   - Push current thread back into its corresponding queue
        //   - Context switch into next thread in readyQueue
        else if (currentThread->state == VM_THREAD_STATE_RUNNING || currentThread->state == VM_THREAD_STATE_READY)
        {
          //cerr << "  running or ready" << endl;
          //if (currentThread == idleThread) cerr << " Pushing idleThread back into readyQueue" << endl;
          if (currentThread->priority == VM_THREAD_PRIORITY_HIGH) highReadyQueue.push_back(currentThread);
          else if (currentThread->priority == VM_THREAD_PRIORITY_NORMAL) normalReadyQueue.push_back(currentThread);
          else if (currentThread != idleThread) lowReadyQueue.push_back(currentThread);
          TCB *oldThread = currentThread;
          currentThread = theQueue->front();
          theQueue->pop_front();
          currentThread->state = VM_THREAD_STATE_RUNNING;
          MachineContextSwitch( &oldThread->context, &currentThread->context );
        }//else if   
      } // Else
    }//if
  } // Scheduler

  // AlarmCallback
  //   - For main Thread
  //     - Decriment ticks if needed
  //     - If ticks == 0 and state is waiting, change to READY and enqueue to normal readyQueue
  //     - Schedule()
  //   - For all items in allOtherThreads
  //     - Decriment ticks if needed
  //     - If ticks == 0 and state is waiting, change to READY and enqueue to corresponding readyQueue
  //     - Schedule()
  void AlarmCallback(void *param)
  {
    // Main Thread: If needed for both: Decriment, Set to ready and pushBack
    /*if (vmMainThread->ticks > 0)
      {
        vmMainThread->ticks -- ;
        if (vmMainThread->ticks == 0 && vmMainThread->state == VM_THREAD_STATE_WAITING)
        {
          vmMainThread->state = VM_THREAD_STATE_READY;
          normalReadyQueue.push_back(vmMainThread);
          Schedule();
        }
      }*/
    // Waitlist
    for (int i=0; i<allOtherThreads.size(); i++)
    {
      // Decriment ticks if needed
      if (allOtherThreads[i]->ticks > 0)
      {
        allOtherThreads[i]->ticks --;
        if (allOtherThreads[i]->ticks == 0 && allOtherThreads[i]->state == VM_THREAD_STATE_WAITING)
        {
          // Set Ready, Push back to corresponding readyQueue
          allOtherThreads[i]->state = VM_THREAD_STATE_READY;
          if (allOtherThreads[i]->priority == VM_THREAD_PRIORITY_HIGH) highReadyQueue.push_back(allOtherThreads[i]);
          else if (allOtherThreads[i]->priority == VM_THREAD_PRIORITY_NORMAL) normalReadyQueue.push_back(allOtherThreads[i]);
          else if (allOtherThreads[i]->priority == VM_THREAD_PRIORITY_LOW) lowReadyQueue.push_back(allOtherThreads[i]);
          Schedule();
        }
      }
    }
    
    for (int i=0; i<allMutexes.size(); i++)
    {
      if (allMutexes[i].lock == 0 && allMutexes[i].threadID != -1)
      {
        int j = 0;
        for (j = 0; j< allOtherThreads.size() && allOtherThreads[j]->threadId != allMutexes[i].threadID; j++ );
        allOtherThreads[j]->state = VM_THREAD_STATE_RUNNING;
        if (allOtherThreads[j]->priority == VM_THREAD_PRIORITY_HIGH) highReadyQueue.push_back(allOtherThreads[j]);
        else if (allOtherThreads[j]->priority == VM_THREAD_PRIORITY_NORMAL) normalReadyQueue.push_back(allOtherThreads[j]);
        else if (allOtherThreads[j]->priority == VM_THREAD_PRIORITY_LOW) lowReadyQueue.push_back(allOtherThreads[j]);
      }
      Schedule();
    }
    
    // Schedule every tick
    //Schedule();
  }
  
  
  void idleFunction(void * param)
  {
    //cerr << "  In Idle" << endl;
    MachineEnableSignals();
    while(1){
    //cerr << "1" << endl;
    }
  }
  
  // SkeletonEntry()
  //   -Run program held in Thread
  //   -After that, terminate the Thread
  //   -Schedule
  void SkeletonEntry(void * param){
    currentThread->entry(param);
    VMThreadTerminate(currentThread->threadId);
    Schedule();
  }

  TVMStatus VMStart(int tickms, TVMMemorySize heapsize, int machinetickms, TVMMemorySize sharedsize, const char *mount, int argc, char *argv[])
  {
    typedef void (*mainPtrFuncType)(int, char*[]);
    mainPtrFuncType mainPtrFunc = VMLoadModule(argv[0]);      // Save inputted program as a function
    
    
    
    if (mainPtrFunc == NULL) return VM_STATUS_FAILURE;        // Make sure inputted program was successful
    MachineEnableSignals();
    
    // Set System Memory Pool
    uint8_t temp;
    TVMMemoryPoolID tempMemory;
    VMMemoryPoolCreate(&temp, heapsize, &tempMemory);
    
    // Create SharedMemory Pool
    uint8_t *sharedBasePointer = (uint8_t*)MachineInitialize( machinetickms, sharedsize );
    memPool *sharedPool = new memPool;
    sharedPool->memoryPoolSize = sharedsize;     // Number of Bytes
    sharedPool->length = sharedsize/ sizeof(uint8_t); // Length of Memory Array
    sharedPool->memoryPoolID = allMemPools.size();
    sharedPool->freeSpace = sharedPool->memoryPoolSize;
    block freeChunk;
    freeChunk.baseIndex = 0;
    freeChunk.length = sharedPool->length;
    sharedPool->freeBlock = freeChunk;
    sharedPool->base = sharedBasePointer;
    allMemPools.push_back(sharedPool);

    
    // Set up Alarm
    MachineRequestAlarm( tickms * 1000, AlarmCallback, NULL); // Request Alarm if needed?
    
    
    // Main Thread: Set Priority and State, set as currentThread, add to readyQueue
    vmMainThread->priority = VM_THREAD_PRIORITY_NORMAL;
    vmMainThread->state = VM_THREAD_STATE_RUNNING;
    vmMainThread->threadId = allOtherThreads.size();
    currentThread = vmMainThread;
    allOtherThreads.push_back(vmMainThread);
    
    
    // Idle Thread: Set Priority and State and Context, add to readyQueue
    idleThread->state = VM_THREAD_STATE_READY;
    VMMemoryPoolAllocate( VM_MEMORY_POOL_ID_SYSTEM, 10000 , (void**)&idleThread->stackptr);
    idleThread->threadId = allOtherThreads.size();
    allOtherThreads.push_back(idleThread);
    MachineContextCreate(&(idleThread->context), idleFunction, NULL, idleThread->stackptr, 100000);
    
    
    //debug();
    
    // Mount File Image and and Allocate data space for storing File Data
    TCB* tempTCB = currentThread;
    tempTCB->state = VM_THREAD_STATE_WAITING;
    tempTCB->fileResult = -1;
    MachineFileOpen( mount, O_RDWR, 0644, fileCallback, tempTCB);
    Schedule();
    mountFd = tempTCB -> fileResult;
    VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SHARED, 512, (void**)&fatData);
    

    ReadBPB();
    loadAllRootEntries();
    loadFatTable();
    
    
    /*for (int i=-2; i<55; i++)
    {
      debug();
    cerr << " Looking at first sector in Cluster " << i+2 << endl;
    ReadSector( globalBPB->FirstDataSector+i*2 );
    PrintCharFat();
    
    }*/
    
    
    if (fatData == NULL)
    {
      cerr <<"  fatData = NULL and tempTCB-> fileResult = " << tempTCB->fileResult <<endl;
      return VM_STATUS_FAILURE;
    }
    
    mainPtrFunc(argc, argv);
    return VM_STATUS_SUCCESS;
  }
  
  
  // VMThreadCreate
  //   - Initialize Thread to defaults
  //   - Add Thread to allOtherThreads list so we can keep track
  TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid)
  {
    // Suspend Machine and save current state
    TMachineSignalState sigstate; // Pointer to hold old State
    MachineSuspendSignals(&sigstate); // Suspend Signals
    
    // Error Checking
    if (entry == NULL || tid == NULL)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    
    // Initialize Thread: Set all items to defaults, push into allOtherThreads List
    TCB *myTCB = new TCB;
    myTCB->state = VM_THREAD_STATE_DEAD;
    myTCB->entry = entry;
    myTCB->param = param;
    myTCB->memsize = memsize;
    myTCB->priority = prio;
    
    // Allocate from System Memory Pool
    //myTCB->stackptr = new uint8_t[memsize];
    VMMemoryPoolAllocate( VM_MEMORY_POOL_ID_SYSTEM, memsize, (void**)&myTCB->stackptr);
    
    *tid = allOtherThreads.size();
    myTCB->threadId = allOtherThreads.size();
    allOtherThreads.push_back(myTCB);
    
    MachineResumeSignals(&sigstate); // Resume Signals
    return VM_STATUS_SUCCESS;
  }
  
  
  TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef state)
  {
    TMachineSignalState sigstate; // Pointer to hold old State
    MachineSuspendSignals(&sigstate); // Suspend Signals (Accepting Interupts)
    
    // Error Checking
    if (allOtherThreads[thread] == NULL)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_ID; // Thread exists?
      
    }
    if (!state) {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    }// State ref is valid?
    
    *state = allOtherThreads[thread]->state; // Set state into the reference given
    
    MachineResumeSignals(&sigstate); // Resume Signals
    return VM_STATUS_SUCCESS;
  }
  
  // VMThreadActivate
  //   - Finds corresponding thread
  //   - Creates a context for that thread
  //   - Set State and push into corresponding readyQueue
  TVMStatus VMThreadActivate(TVMThreadID thread)
  {
    TMachineSignalState sigstate; // Pointer to hold old State
    MachineSuspendSignals(&sigstate); // Suspend Signals (Accepting Interupts)
    
    // Find corresponding thread
    if(allOtherThreads[thread] == NULL)
    {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_ID;
    }
    int i = 0;
    for (i = 0; i< allOtherThreads.size() && allOtherThreads[i]->threadId != thread; i++ );
    
    // Create Context
    MachineContextCreate(&(allOtherThreads[i]->context), SkeletonEntry, allOtherThreads[i]->param, allOtherThreads[i]->stackptr, allOtherThreads[i]->memsize);
    
    // Set State, push into corresponding readyQueue
    allOtherThreads[i]->state = VM_THREAD_STATE_READY;
    if(allOtherThreads[i]->priority == VM_THREAD_PRIORITY_LOW) lowReadyQueue.push_back(allOtherThreads[i]);
    else if(allOtherThreads[i]->priority == VM_THREAD_PRIORITY_NORMAL) normalReadyQueue.push_back(allOtherThreads[i]);
    else if(allOtherThreads[i]->priority == VM_THREAD_PRIORITY_HIGH) highReadyQueue.push_back(allOtherThreads[i]);
    
    if (allOtherThreads[i]->priority > currentThread->priority)
      Schedule();
    MachineResumeSignals(&sigstate); // Resume Signals
    return VM_STATUS_SUCCESS;
  }

  // VMThreadTerminate
  //   - Find thread and error check
  //   - Set threadState to Dead
  //   - Remove thread from Queues
  TVMStatus VMThreadTerminate(TVMThreadID thread)
  {
    TMachineSignalState sigstate; // Pointer to hold old State
    MachineSuspendSignals(&sigstate); // Suspend Signals (Accepting Interupts)
    
    // Find thread and error check
    int i = 0;
    for (i = 0; i< allOtherThreads.size() && allOtherThreads[i]->threadId != thread; i++ );
    if (i == allOtherThreads.size() ) {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_ID;
    }
    if (allOtherThreads[i]->state == VM_THREAD_STATE_DEAD){
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_STATE;
    }
    
    // Set state, remove thread from Queues
    allOtherThreads[i]->state = VM_THREAD_STATE_DEAD;
    lowReadyQueue.remove(allOtherThreads[i]);
    normalReadyQueue.remove(allOtherThreads[i]);
    highReadyQueue.remove(allOtherThreads[i]);
    
    MachineResumeSignals(&sigstate); // Resume Signals
    return VM_STATUS_SUCCESS;
  }
  
  // VMThreadSleep()
  //   -
  TVMStatus VMThreadSleep(TVMTick tick)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    // Error Checking
    if (tick == VM_TIMEOUT_INFINITE) {
      MachineResumeSignals(&sigstate);
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    }    // If ticks was set to < 0, Infinite Timeout
    if ( currentThread == vmMainThread )
    {
      //cerr << "  " << tick << "Main about to sleep" << endl;
      
      vmMainThread->ticks = tick;
      vmMainThread->state = VM_THREAD_STATE_WAITING;
      if (currentThread->priority == VM_THREAD_PRIORITY_HIGH) highReadyQueue.remove(vmMainThread);
      else if (currentThread->priority == VM_THREAD_PRIORITY_NORMAL) normalReadyQueue.remove(vmMainThread);
      else if (currentThread->priority == VM_THREAD_PRIORITY_LOW) lowReadyQueue.remove(vmMainThread);
      //else //cerr << "  Undefined priority" << endl;
      vmMainThread->ticks = tick;
      Schedule();
    }
    else
    {
      //cerr << "  Multiple threads detected in sleep" << endl;
      currentThread->ticks = tick;
      currentThread->state = VM_THREAD_STATE_WAITING;
      if (currentThread->priority == VM_THREAD_PRIORITY_HIGH) highReadyQueue.remove(currentThread);
      else if (currentThread->priority == VM_THREAD_PRIORITY_NORMAL) normalReadyQueue.remove(currentThread);
      else if (currentThread->priority == VM_THREAD_PRIORITY_LOW) lowReadyQueue.remove(currentThread);
      //else //cerr << "  Undefined priority" << endl;
      Schedule();
      //cerr << "  currentThread->ticks =" << currentThread->ticks<<endl;
    }
    
    //cerr << "  About to exit sleep" << endl;
    MachineResumeSignals(&sigstate);
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMThreadDelete(TVMThreadID thread)
  {
    int i;
    for (i = 0; i< allOtherThreads.size() && allOtherThreads[i]->threadId != thread; i++ );
    if (i == allOtherThreads.size())
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    else if (allOtherThreads[i]->state != VM_THREAD_STATE_DEAD)
      return VM_STATUS_ERROR_INVALID_STATE;

    allOtherThreads.erase(allOtherThreads.begin() + i);
    return VM_STATUS_SUCCESS;

  }
  
  void fileCallback(void *calldata, int result)
  {

    TCB * tempTCB = (TCB*) calldata;
    tempTCB->state = VM_THREAD_STATE_READY;

    if(tempTCB->priority == VM_THREAD_PRIORITY_HIGH)
      highReadyQueue.push_back(tempTCB);
    else if (tempTCB->priority == VM_THREAD_PRIORITY_NORMAL)
      normalReadyQueue.push_back(tempTCB);
    else
      lowReadyQueue.push_back(tempTCB);
    tempTCB->fileResult = result;
    //cerr << " Result = " << result << endl;
    //if (result < 0 ) cerr << " MachineFileWrite failed " << endl;
    Schedule();
  }

  TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor)
  {
    // NOTE: YOU NEED TO ADD FILEDESCRIPTOR == 1 0R 0 TO RUN OLD IMPLEMENTATION SO FILE.SO WILL STILL WORK
    
    
    //cerr <<"In FileOpen"<<endl;
    TMachineSignalState sigstate; // Pointer to hold old State
    MachineSuspendSignals(&sigstate); // Suspend Signals
    
    //TCB * it = currentThread;
    if (filename == NULL || filedescriptor == NULL)
    {
      MachineResumeSignals(&sigstate); // Resume Signals
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    
    
    // Look for Corresponding File:
    //   If found, return filedescriptor = index + 2
    //   Else, make a new file and return file descriptor of that
    //cerr << " comparing :" << filename << ":" << endl;
    int j;
    for (j=0; j< rootTable.size(); j++)
    {
      //cerr << "   To:" << rootTable[j]->DIR_Name << ":" << endl;
      int len = strlen(filename);
      //cerr << " len =" << len << endl;
      
      if ( memcmp( rootTable[j]->DIR_Name, filename, len ) == 0 ) break;
    }
    if (j != rootTable.size())
    {
      *filedescriptor = j+2;
      //cerr << " Open success: Returned fileDescriptor " << *filedescriptor << endl;
      //cerr << " Corresponding index = " << j << endl;
    }
    else
    {
      //cerr << " File not found " << endl;
      MachineResumeSignals(&sigstate);
      return VM_STATUS_FAILURE;
      /*cerr << " New file made, this is not supported ahhahaahh " << endl;
      *filedescriptor = j+2;
      
      // Make new file.
      rootEntry *newFile = new rootEntry;
      newFile->DIR_Name = new uint8_t[11];
      memcpy ( newFile->DIR_Name, filename, 11);
      
      rootTable.push_back( newFile); */
      
      
    }
    
    
    
    /*
     
     TCB* tempTCB = currentThread;
     tempTCB->state = VM_THREAD_STATE_WAITING;
     tempTCB->fileResult = -1;
     
     
     MachineFileOpen(filename, flags, mode, fileCallback, tempTCB);
     //cerr << "VMFileOpen calling Schedule"<<endl;
     Schedule();
     
     //cerr << "currentThread->fileResult = "<<tempTCB->fileResult<<endl;
     if (tempTCB->fileResult < 0)
     {
     MachineResumeSignals(&sigstate); // Resume Signals
     return VM_STATUS_FAILURE;
     }
     
     *filedescriptor = tempTCB->fileResult;
     
     */
    
    
    
    MachineResumeSignals(&sigstate); // Resume Signals
    return VM_STATUS_SUCCESS;
  }

  TVMStatus VMFileClose(int filedescriptor)
  {
    TMachineSignalState sigstate;
    MachineSuspendSignals(&sigstate);
    cerr << "-------------------------------------------" << endl;
    cerr << " filedescriptor =" << filedescriptor << endl;
    
    if (filedescriptor == 1 || filedescriptor == 0)
    {

      MachineFileClose(filedescriptor, fileCallback, currentThread);
      currentThread -> state = VM_THREAD_STATE_WAITING;
      if (currentThread->fileResult < 0)
      {
        MachineResumeSignals(&sigstate);
        return VM_STATUS_FAILURE;
      }
    }
    MachineResumeSignals(&sigstate);
    return VM_STATUS_SUCCESS;
  }

  
  TVMStatus VMFileWrite(int filedescriptor, void *data, int *length)
  {
    //cerr<<"IN VMFILEWrite-------------------------------------------"<<endl;
    TMachineSignalState sigstate; // Pointer to hold old State
    MachineSuspendSignals(&sigstate); // Suspend Signals
    // For Regular Writes
    
    //cerr << " About to VMFileWrite " << *length << endl;
    if (filedescriptor < 3)
    {
      if (data == NULL || length== NULL )
      {
        MachineResumeSignals(&sigstate); // Resume Signals
        return VM_STATUS_ERROR_INVALID_PARAMETER;
      }
      TCB* tempTCB = currentThread;
      if (*length <= 512)
      {
        tempTCB->state = VM_THREAD_STATE_WAITING; 
        uint8_t *newlyAllocatedBase;
        VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SHARED, *length, (void**)&newlyAllocatedBase);
        memcpy( newlyAllocatedBase, data, *length);
        MachineFileWrite(filedescriptor, newlyAllocatedBase, *length, fileCallback, tempTCB);
        Schedule();
      }
      else
      {
         //cerr <<data<<endl;
        uint8_t *newlyAllocatedBase;
        VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SHARED, *length, (void**)&newlyAllocatedBase);
        tempTCB->state = VM_THREAD_STATE_WAITING;
        memcpy( newlyAllocatedBase, data, *length);
        for (int i = 512; i <= *length; )
        {
          MachineFileWrite(filedescriptor, &newlyAllocatedBase[i-512], 512, fileCallback, tempTCB);
          
          i += 512;
        }
        Schedule();
      }
      
      if (tempTCB->fileResult < 0)
      {
        MachineResumeSignals(&sigstate); // Resume Signals
        return VM_STATUS_FAILURE;
      }
      *length = tempTCB->fileResult;
    }
    //-------------Project 4--------------//
    //             Writing from
    else
    {

    }
    
    MachineResumeSignals(&sigstate); // Resume Signals
    return VM_STATUS_SUCCESS;
  }

  TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset)
  {
    TMachineSignalState sigstate; // Pointer to hold old State
    MachineSuspendSignals(&sigstate); // Suspend Signals

    if (newoffset == NULL)
    {
      MachineResumeSignals(&sigstate); // Resume Signals
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    
    TCB* tempTCB = currentThread;
    tempTCB->state = VM_THREAD_STATE_WAITING;
    tempTCB->fileResult = -1;
    
    MachineFileSeek(filedescriptor, offset, whence, fileCallback, tempTCB);
    Schedule();

    //cerr << "currentThread->fileResult = "<<tempTCB->fileResult<<endl;
    if (tempTCB->fileResult < 0)
    {
      MachineResumeSignals(&sigstate); // Resume Signals
      return VM_STATUS_FAILURE;
    }
          
    *newoffset = tempTCB->fileResult;

    MachineResumeSignals(&sigstate); // Resume Signals
    return VM_STATUS_SUCCESS;
  }

  TVMStatus VMFileRead(int filedescriptor, void *data, int *length)
  {
    TMachineSignalState sigstate; // Pointer to hold old State
    MachineSuspendSignals(&sigstate); // Suspend Signals
    cerr << " VMFileRead: filedescriptor=" << filedescriptor << " and length =" << *length << endl;
    
    // Regular FileRead
    if (filedescriptor == 0 || filedescriptor == 1)
    {
      if (data == NULL || length== NULL )
      {
        MachineResumeSignals(&sigstate); // Resume Signals
        return VM_STATUS_ERROR_INVALID_PARAMETER;
      }
      
      //cerr<< "Regular fileRead filedesc =" << filedescriptor << endl;
      TCB* tempTCB = currentThread;
      
      uint8_t *newlyAllocatedBase;
      VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SHARED, *length, (void**)&newlyAllocatedBase);
      MachineFileRead(filedescriptor, newlyAllocatedBase, *length, fileCallback, tempTCB);
      
      tempTCB->state = VM_THREAD_STATE_WAITING;
      
      Schedule();

      //cerr << "currentThread->fileResult = "<<tempTCB->fileResult<<endl;
      if (tempTCB->fileResult < 0)
      {
        MachineResumeSignals(&sigstate); // Resume Signals
        return VM_STATUS_FAILURE;
      }
      
      memcpy(data, newlyAllocatedBase, *length);
      
      *length = tempTCB->fileResult;
    }
    
    //---------------  Program 4 ----------------//
    //          FileRead for FAT Files           //
    //-------------------------------------------//
    // 1. Use fileDescriptor to find the corresponding file in the rootTable
    // 2. Use clusterLo to find information to access, Read Information into Buffer
    // 3. Use FatTable to find next corresponding Clusters.
    else
    {
      //cerr << " FileREad " << endl;
      int index = filedescriptor - 2; // Index in RootEntry Vector is fd-2
      //cerr << " Index =" << index << endl;
      //cerr << " fileDescriptor =" << filedescriptor << endl;
      
      //index = 4;
      
      // Debugging
      if (data == NULL || length == NULL) return VM_STATUS_ERROR_INVALID_PARAMETER;
      if (rootTable[index]->DIR_Attr == 0x10)
      {
        //cerr << " Directory selected??" << endl;
        MachineResumeSignals(&sigstate);
        return VM_STATUS_FAILURE;
      }
      
      uint16_t firstCluster = rootTable[index]->DIR_FstClusLO;
      //uint16_t lastCluster = rootTable[index]->DIR_FstClusHI;

      //cerr << currentReadCluster << endl;
    
      // If this is the first cluster
      if (currentReadCluster == -1)
      {
        cerr << " firstclus" << endl;
        currentReadCluster = firstCluster;
        ReadCluster(currentReadCluster);
        memcpy( data, fatData, 512*2);
        
        currentReadCluster = fatTable[0]->clusterChain[firstCluster]->nextCluster;
        //*length = 1024;
        MachineResumeSignals(&sigstate); // Resume Signals
        return VM_STATUS_SUCCESS;
      }
      else if (currentReadCluster >= 65535)
      {
        cerr << " lastclus" << endl;
        ReadCluster(currentReadCluster);
        memcpy( data, fatData, 512*2);
        
        currentReadCluster = fatTable[0]->clusterChain[currentReadCluster]->nextCluster;
        //*length = 1024;
        MachineResumeSignals(&sigstate);
        return VM_STATUS_FAILURE;
        
      }
      else
      {
        cerr << " midclus" << endl;
        ReadCluster(currentReadCluster);
        memcpy( data, fatData, 512*2);
        
        currentReadCluster = fatTable[0]->clusterChain[currentReadCluster]->nextCluster;
        //*length = 512*2;
        MachineResumeSignals(&sigstate); // Resume Signals
        return VM_STATUS_SUCCESS;
        
      }
      
      
      
      
    }
      
    MachineResumeSignals(&sigstate); // Resume Signals
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMMutexCreate(TVMMutexIDRef mutexref)
  {
    //unsigned int *returnedRef= new unsigned int;
    //*returnedRef = (unsigned int)allMutexes.size();
    //cerr << "  "<< allMutexes.size() << " mutexes now exist" << endl;
    *mutexref = (unsigned int)allMutexes.size();
    mutexHolder newMutex;
    newMutex.lock = 0;
    newMutex.threadID = -1;
    allMutexes.push_back(newMutex);
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout)
  {
    TMachineSignalState sigstate; // Pointer to hold old State
    MachineResumeSignals(&sigstate);
    //cerr << "  Listing all Mutexes and their states" << endl;
    for (int i = 0; i < allMutexes.size(); i++)
    {
      //cerr << "  Mutex " << i << " state = " << allMutexes[i] << endl;
    }
    
    // If I want to know right now
    if (timeout == VM_TIMEOUT_IMMEDIATE)
    {
      // If Mutex is free, lock it and return success!
      // Else, return failure
      if (allMutexes[(int)mutex].lock == 0)
      {
        //cerr << "  Mutex Free: aquired! #" << mutex << endl;
        allMutexes[(int)mutex].lock = 1;
        MachineResumeSignals(&sigstate);
        return VM_STATUS_SUCCESS;
      }
      else
      {
        //cerr << "  Mutex Not Free: not aquired #" << mutex << endl;
        MachineResumeSignals(&sigstate);
        return VM_STATUS_FAILURE;
      }
    }
    
    // If I'm ok with waiting
    else if(timeout == VM_TIMEOUT_INFINITE)
    {
      int index = (int)mutex;
      //if (allMutexes[index].lock == 0) cerr << "  No need to wait for Mutex #" << index << endl;
      //else cerr << "  Waiting for Mutex #" << index << endl;
      if (allMutexes[index].lock == 1)
      {
        currentThread->state = VM_THREAD_STATE_WAITING;
        allMutexes[index].threadID = currentThread->threadId;
        Schedule();
      }
      while (allMutexes[index].lock == 1)
      {}
      //currentThread->state = VM_THREAD_STATE_RUNNING;
      currentThread->ticks = 0;
      //cerr << "  Wait ended: Aquired Mutex! #" << index << endl;
      allMutexes[index].lock = 1;
      MachineResumeSignals(&sigstate);
      return VM_STATUS_SUCCESS;
    }
    else{
      MachineResumeSignals(&sigstate);
      return VM_STATUS_FAILURE;
    }
  }
  
  TVMStatus VMMutexRelease(TVMMutexID mutex)
  {
    //cerr << "  Trying to release Mutex #" << mutex << endl;
    allMutexes[(int)mutex].lock = 0;
    return VM_STATUS_SUCCESS;
  }
  

  TVMStatus VMMemoryPoolCreate(void *base, TVMMemorySize size, TVMMemoryPoolIDRef memory)
  {
    //cerr << "In pool create" << endl;
    // Error Checking
    if (base == NULL || memory == NULL || size == 0){
      //cerr << "Pool Create: Invalid Param" << endl;
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    
    TMachineSignalState sigstate;
    MachineResumeSignals(&sigstate);
    
    // Return poolID
    *memory = allMemPools.size();
    
    
    // Initialize Pool
    memPool *newPool = new memPool;
    newPool->memoryPoolSize = size;      // Number of Bytes
    newPool->length = size/ sizeof(uint8_t); // Length of Memory Array
    //newPool->base = base;
    newPool->memoryPoolID = allMemPools.size();
    newPool->freeSpace = newPool->memoryPoolSize;
    block freeChunk;
    freeChunk.baseIndex = 0;
    freeChunk.length = newPool->length;
    newPool->freeBlock = freeChunk;
    newPool->base = new uint8_t[ size/sizeof(uint8_t)];
    //cerr << " Initialized length of memory Ar	ray = " << size/sizeof(uint8_t) << " and " << newPool->length << endl;
    base = newPool->base;
    allMemPools.push_back(newPool);
    //cerr << allMemPools.size() << "Is mempool size" << endl;
    
    MachineResumeSignals(&sigstate); // Resume Signals
    return VM_STATUS_SUCCESS;
    
  }
  
  TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer)
  {
    // Add blocks to Allocated list
    block *tempBlock = new block;
    tempBlock->baseIndex = allMemPools[memory]->freeBlock.baseIndex;
    tempBlock->length = size/sizeof(uint8_t);
    allMemPools[memory]->allocatedList.push_back(tempBlock);

    // alter block in free list
    allMemPools[memory]->freeBlock.baseIndex += size/sizeof(uint8_t);
    allMemPools[memory]->freeBlock.length -= size/sizeof(uint8_t);

    //cerr << allMemPools[memory]->base[60] << endl;
    //cerr << tempBlock->baseIndex << endl;
    uint8_t * tempBase = &allMemPools[memory]->base[tempBlock->baseIndex];
    *pointer = tempBase;
    return VM_STATUS_SUCCESS;
  }
  
  
  TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef bytesleft)
  {
    int i;
    for (i = 0; i< allMemPools.size() && allMemPools[i]->memoryPoolID != memory; i++ );
    if (i == allMemPools.size() || bytesleft == NULL)
      return VM_STATUS_ERROR_INVALID_PARAMETER;

    TVMMemorySize freeSpace = allMemPools[i]->memoryPoolSize - (TVMMemorySize) allMemPools[i]->length;
    bytesleft = &freeSpace;

    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory)
  {
    int i;
    for (i = 0; i< allMemPools.size() && allMemPools[i]->memoryPoolID != memory; i++ );
    if (i == allMemPools.size())
      return VM_STATUS_ERROR_INVALID_PARAMETER;

    if (allMemPools[i]->allocatedList.size() > 0)
      return VM_STATUS_ERROR_INVALID_STATE;

    allMemPools.erase(allMemPools.begin() + i);
    return VM_STATUS_SUCCESS;

    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void *pointer)
  {
    block *tempBlock = allMemPools[memory]->allocatedList.front();
    allMemPools[memory]->allocatedList.pop_front();

    allMemPools[memory]->freeBlock.baseIndex = 0;
    allMemPools[memory]->freeBlock.length += tempBlock->length;

    return VM_STATUS_SUCCESS;
  }
  
  /*
   -------------------------------------------------------------------------------------------------------------
      Project 4
      5/25/2015
   -------------------------------------------------------------------------------------------------------------
   */
  
  TVMStatus VMDirectoryOpen(const char *dirname, int *dirdescriptor)
  {
    //cerr << "VMDirectoryOpen Called"<< endl;
    
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMDirectoryClose(int dirdescriptor)
  {
    
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMDirectoryRead(int dirdescriptor, SVMDirectoryEntryRef dirent)
  {
    TMachineSignalState sigstate; // Pointer to hold old State
    MachineSuspendSignals(&sigstate);
    
    if (dirReadCounter != rootTable.size())
    {
      *dirent = rootTable[dirReadCounter]->SVMDirEntry;
      
      /*
      rootEntry *newEntry = new rootEntry;
      newEntry->SVMDirEntry = *dirent;
      VMPrint("%04d/%02d/%02d %02d:%02d %s ",newEntry->SVMDirEntry.DModify.DYear, newEntry->SVMDirEntry.DModify.DMonth, newEntry->SVMDirEntry.DModify.DDay, (newEntry->SVMDirEntry.DModify.DHour % 12) ? (newEntry->SVMDirEntry.DModify.DHour % 12) : 12 , newEntry->SVMDirEntry.DModify.DMinute, newEntry->SVMDirEntry.DModify.DHour >= 12 ? "PM" : "AM");
      VMPrint("%s ", newEntry->SVMDirEntry.DAttributes & VM_FILE_SYSTEM_ATTR_DIRECTORY ? "<DIR> " : "<FILE>");*/
      
      
      dirReadCounter++;
      //cerr << dirReadCounter << " is the dirREadCounter " << endl;
    }
    else
    {
      dirReadCounter = 0;
      MachineResumeSignals(&sigstate); // Resume Signals
      return VM_STATUS_FAILURE;
    }
    MachineResumeSignals(&sigstate); // Resume Signals
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMDirectoryRewind(int dirdescriptor)
  {
    
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMDirectoryCurrent(char *abspath)
  {
    /*
    if ( memcmp( ) != 0 )
    {
      memcpy(abspath, &currentDirectory, 11);
    }
    else
    {
      *abspath = "/";
    } */
    //char temp[] = "/";
    //*abspath = temp
    if (abspath == NULL) return VM_STATUS_ERROR_INVALID_PARAMETER;
    char temp[] = "/";
    memcpy( abspath, temp, 11);
    
    return VM_STATUS_SUCCESS;
    
  }
  
  TVMStatus VMDirectoryCreate(const char *dirname)
  {
    if (dirname == NULL)
      return VM_STATUS_ERROR_INVALID_PARAMETER;
    rootEntry *newDirectory = new rootEntry;
    newDirectory->DIR_Name = new uint8_t[11];
    memcpy( newDirectory->SVMDirEntry.DShortFileName, dirname, 11);
    VMDateTime(&newDirectory->SVMDirEntry.DModify);
    rootTable.push_back(newDirectory);
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMDirectoryChange(const char *path)
  {
    /*if (path == NULL) return VM_STATUS_INVALID_PARAMETER;
    
    // Find Corresponding Directory
    int j;
    for (j=0; j< rootTable.size(); j++)
    {
      if ( memcmp( rootTable[j]->DIR_Name, path, 11) == 0 ) break;
    }
    // If found...
    if (j != rootTable.size())
    {
      // load directory information from clusters into the currentDirecctories information.
    }
    else
    {
      return VM_STATUS_INVALID_PARAMETER;
    }
    */
    
    return VM_STATUS_SUCCESS;
  }
  
  TVMStatus VMDirectoryUnlink(const char *path)
  {
    
    return VM_STATUS_SUCCESS;
  }
  
  uint16_t EndiantoInt( int numBytes, uint8_t* base)
  {
    uint16_t sum = 0;
    for (int i=0; i<numBytes; i++)
    {
      sum += (uint16_t)base[i]* exp2(8*i);
    }
    return sum;
  }
  
  void ReadSector (int sectorInt)
  {
    TCB* tempTCB = currentThread;
    tempTCB->state = VM_THREAD_STATE_WAITING;
    /*tempTCB->fileResult = -1;
    MachineFileOpen( mount, O_RDWR, 0644, fileCallback, tempTCB);
    Schedule(); */
     
    //int fd = tempTCB->fileResult;
    //cerr <<"IN READSECTOR: fd = "<<fd<<endl;
    MachineFileSeek(mountFd, 512*sectorInt, 0, fileCallback, tempTCB);
    Schedule();
    
    tempTCB->state = VM_THREAD_STATE_WAITING;
    MachineFileRead(mountFd, fatData, 512, fileCallback, tempTCB);
    Schedule();
    
    //tempTCB->fileResult = fd;
  }
  
  void ReadCluster(int clusterInt)
  {
    uint8_t readIn[512*2];
    TCB* tempTCB = currentThread;
    tempTCB->state = VM_THREAD_STATE_WAITING;

    int startingSector = globalBPB->FirstDataSector + (clusterInt-2)*2;

    //cerr << " startingsec calculated by readClust =" << startingSector << endl;
    
    //int fd = tempTCB->fileResult;
    tempTCB->fileResult = mountFd;
    //cerr << "mountFd= "<<mountFd<<endl;
    ReadSector(startingSector);
    memcpy(readIn, fatData, 512);
    ReadSector(startingSector + 1);
    //strcat((char *) readIn, (char *) fatData);
    memcpy(&readIn[512], fatData, 512);
    memcpy(fatData, readIn, 512*2);
   
    //for (int i = 0; i < BPB->BPB_SecPerClus; i++)
    //{
      /*cerr << "clusterInt = "<<clusterInt <<"\tstartingSector = "<<startingSector<<endl;
      MachineFileSeek(mountFd, 512*startingSector, 0, fileCallback, tempTCB);
      Schedule();
      
      tempTCB->state = VM_THREAD_STATE_WAITING;
      MachineFileRead(mountFd, fatData, 512*2, fileCallback, tempTCB);
      Schedule();*/
    //}
    
      //cerr <<"IN READCLUSTER: --------------------------------------------------"<<endl;
      //PrintCharFat();
    //tempTCB->fileResult = fd;
  }
  
  void ReadBPB()
  {
    globalBPB = new BPB;

    ReadSector(0);
    

    globalBPB->BS_jmpBoot = EndiantoInt(3, &fatData[0]);
    
    globalBPB->BS_OEMName = new uint8_t[8];
    memcpy(globalBPB->BS_OEMName, &fatData[3], 8);

    globalBPB->BPB_BytesPerSec = EndiantoInt(2, &fatData[11]);
    globalBPB->BPB_SecPerClus = EndiantoInt(1, &fatData[13]);
    globalBPB->BPB_RsvdSecCnt = EndiantoInt(2, &fatData[14]);
    globalBPB->BPB_NumFATs = EndiantoInt(1, &fatData[16]);
    globalBPB->BPB_RootEntCnt = EndiantoInt(2, &fatData[17]);
    globalBPB->BPB_TotSec16 = EndiantoInt(2, &fatData[19]);
    globalBPB->BPB_Media = EndiantoInt(1, &fatData[21]);
    globalBPB->BPB_FATSz16 = EndiantoInt(2, &fatData[22]);
    globalBPB->BPB_SecPerTrk = EndiantoInt(2, &fatData[24]);
    globalBPB->BPB_NumHeads = EndiantoInt(2, &fatData[26]);
    globalBPB->BPB_HiddSec = EndiantoInt(4, &fatData[28]);
    globalBPB->BPB_TotSec32 = EndiantoInt(4, &fatData[32]);
    globalBPB->BS_DrvNum = EndiantoInt(1, &fatData[36]);
    globalBPB->BS_Reserved1 = EndiantoInt(1, &fatData[37]);
    globalBPB->BS_BootSig = EndiantoInt(1, &fatData[38]);
    globalBPB->BS_VolID = EndiantoInt(4, &fatData[39]);

    
    //memcpy(globalBPB->BS_VolLab, &fatData[43], sizeof(fatData)/512*11);
    //memcpy(globalBPB->BS_FilSysType, &fatData[54], sizeof(fatData)/512*8);
    
    
    globalBPB->FirstRootSector = globalBPB->BPB_RsvdSecCnt + globalBPB->BPB_NumFATs * globalBPB->BPB_FATSz16;
    globalBPB->RootDirectorySectors = (globalBPB->BPB_RootEntCnt * 32)/512 ;
    globalBPB->FirstDataSector = globalBPB->FirstRootSector + globalBPB->RootDirectorySectors;
    globalBPB->ClusterCount  = (globalBPB->BPB_TotSec32 - globalBPB->FirstDataSector) / globalBPB->BPB_SecPerClus ;
    
  }
  
  void CheckBPB()
  {
    cerr << "BSjmpBoot = " << globalBPB->BS_jmpBoot << endl;
    cerr << "BS_OEMName = " << globalBPB->BS_OEMName << endl;
    
    cerr << "BPB_BytesPerSec = " << globalBPB->BPB_BytesPerSec << endl;
    cerr << "BPB_SecPerClus = " << globalBPB->BPB_SecPerClus << endl;
    cerr << "BPB_RsvdSecCnt = " << globalBPB->BPB_RsvdSecCnt << endl;
    cerr << "BPB_NumFATs = " << globalBPB->BPB_NumFATs << endl;
    cerr << "BPB_RootEntCnt = " << globalBPB->BPB_RootEntCnt << endl;
    cerr << "BPB_TotSec16 = " << globalBPB->BPB_TotSec16 << endl;
    cerr << "BPB_Media = " << globalBPB->BPB_Media << endl;
    cerr << "BPB_FATSz16 = " << globalBPB->BPB_FATSz16 << endl;
    cerr << "BPB_SecPerTrk = " << globalBPB->BPB_SecPerTrk << endl;
    cerr << "BPB_NumHeads = " << globalBPB->BPB_NumHeads << endl;
    cerr << "BPB_HiddSec = " << globalBPB->BPB_HiddSec << endl;
    cerr << "BPB_TotSec32 = " << globalBPB->BPB_TotSec32 << endl;
    cerr << "BPB_DrvNum = " << globalBPB->BS_DrvNum << endl;
    cerr << "BPB_Reserved1 = " << globalBPB->BS_Reserved1 << endl;
    cerr << "BPB_BootSig = " << globalBPB->BS_BootSig << endl;
    cerr << "BPB_VolID= " << globalBPB->BS_VolID << endl;
    //cerr << "BPB_VolLab = " << globalBPB->BS_VolLab << endl;
    //cerr << "BPB_FilSysType = " << globalBPB->BS_FilSysType << endl;
    
  }
  
  void PrintCharFat()
  {
    for (int i=0; i<512; i++)
    {
      cerr <<fatData[i];
    }
  }
  
  void PrintNumFat()
  {
    for (int i=0; i<512; i++)
    {
      cerr << i << " " << (int)fatData[i]<<endl;
    }
  }
  


  void loadSVMDateTime(uint16_t storeDate, uint16_t storeTime, SVMDateTimeRef storeDateTime)
  {
    storeDateTime->DYear = (storeDate >> 9) + 1980;
    storeDateTime->DMonth = (storeDate >> 5) & 0xF;
    storeDateTime->DDay = storeDate & 0x1F;
    
    storeDateTime->DHour = ( storeTime >> 11);
    storeDateTime->DMinute = ( storeTime >> 5) & 0x3F;
    storeDateTime->DSecond = storeTime & 0x1F;
  }
  
  void loadAllRootEntries()
  {
    cerr << " New LoadRoot" << endl;
    uint8_t* fileName;
    ReadSector( globalBPB->FirstRootSector);
    setZeroDebug();
    PrintCharFat();
    //PrintNumFat();
    
    rootEntry* newEntry;
    for (int i=0; i< globalBPB->BPB_RootEntCnt / 32; i++)
    {
      if ( EndiantoInt(1, &fatData[i*32+ 11]) != 15)
      {
        debug();
        newEntry = new rootEntry;
        
        newEntry->DIR_Name = new uint8_t[11];
        memcpy(newEntry->DIR_Name, &fatData[i*32], 8);//11);
        fileName = (uint8_t *) strtok((char *) newEntry->DIR_Name, " ");
        if (fatData[i*32 + 8] != ' ')
        {
          strcat((char *) fileName, ".");
          //memcpy(newEntry->DIR_Name, &fatData[i*32 + 8], 3);
          strncat((char *) fileName, (char *) &fatData[i*32 + 8], 3);
        }
        memcpy(newEntry->DIR_Name, fileName, 11);
        newEntry->DIR_Attr = EndiantoInt(1, &fatData[i*32 + 11]);
        newEntry->DIR_NTRes = EndiantoInt(1, &fatData[i*32 + 12]);
        newEntry->DIR_CrtTimeTenth = EndiantoInt(1, &fatData[i*32 + 13]);
        newEntry->DIR_CrtTime = EndiantoInt(2, &fatData[i*32 + 14]);
        newEntry->DIR_CrtDate = EndiantoInt(2, &fatData[i*32 + 16]);
        newEntry->DIR_LstAccDate = EndiantoInt(2, &fatData[i*32 + 18]);
        newEntry->DIR_FstClusHI = EndiantoInt(2, &fatData[i*32+ 20]);
        newEntry->DIR_WrtTime = EndiantoInt(2, &fatData[i*32 + 22]);
        newEntry->DIR_WrtDate = EndiantoInt(2, &fatData[i*32 + 24]);
        newEntry->DIR_FstClusLO = EndiantoInt(2, &fatData[i*32+ 26]);
        newEntry->DIR_FileSize = EndiantoInt(4, &fatData[i*32+ 28]);
        
        cerr<<"DIR_Name = "<<newEntry->DIR_Name<<"DIR_Attr = "<<newEntry->DIR_Attr<<"\nDIR_NTRes = "<<newEntry->DIR_NTRes<<"\nDIR_CrtTimeTenth = "<<newEntry->DIR_CrtTimeTenth<<
         "\nDIR_CrtTime = "<<newEntry->DIR_CrtTime<<"\nDIR_CrtDate = "<<newEntry->DIR_CrtDate<<"\nDIR_LstAccDate = "<<newEntry->DIR_LstAccDate<<
         "\nDIR_FstClusHI = "<<newEntry->DIR_FstClusHI<<"\nDIR_WrtTime = "<<newEntry->DIR_WrtTime<<"\nDIR_WrtDate = "<<newEntry->DIR_WrtDate<<"\nDIR_FstClusLO = "<<newEntry->DIR_FstClusLO<<
         "\nDIR_FileSize = "<<newEntry->DIR_FileSize<<endl; 
        
        //cerr <<"loading Date"<<endl;
        loadSVMDateTime(newEntry->DIR_CrtDate, newEntry->DIR_CrtTime, &(newEntry->SVMDirEntry.DCreate));
        //loadSVMDate(newEntry->DIR_LstAccDate, &(newEntry->SVMDirEntry.DAccess));
        loadSVMDateTime(newEntry->DIR_WrtDate, newEntry->DIR_WrtTime, &(newEntry->SVMDirEntry.DModify));
        
        /*
         cerr <<"DCreate.DYear = "<<newEntry->SVMDirEntry.DCreate.DYear<<
         "\tDCreate.DMonth = "<< (unsigned int) newEntry->SVMDirEntry.DCreate.DMonth<<
         "\tDCreate.DDay = "<<(unsigned int)newEntry->SVMDirEntry.DCreate.DDay<<
         "\nDCreate.DHour = "<<(unsigned int)newEntry->SVMDirEntry.DCreate.DHour<<
         "\tDCreate.DMinute = "<< (unsigned int) newEntry->SVMDirEntry.DCreate.DMinute<<
         "\tDCreate.DSecond = "<<(unsigned int)newEntry->SVMDirEntry.DCreate.DSecond<<endl;
         /*cerr <<"DAccess.DYear = "<<newEntry->SVMDirEntry.DAccess.DYear<<
         "\tDAccess.DMonth = "<< (unsigned int) newEntry->SVMDirEntry.DAccess.DMonth<<
         "\tDAccess.DDay = "<<(unsigned int)newEntry->SVMDirEntry.DAccess.DDay<<endl;*/
        
        /*
         cerr <<"DModify.DYear = "<<newEntry->SVMDirEntry.DModify.DYear<<
         "\tDModify.DMonth = "<< (unsigned int) newEntry->SVMDirEntry.DModify.DMonth<<
         "\tDModify.DDay = "<<(unsigned int)newEntry->SVMDirEntry.DModify.DDay<<
         "\nDModify.DHour = "<<(unsigned int)newEntry->SVMDirEntry.DModify.DHour<<
         "\tDModify.DMinute = "<< (unsigned int) newEntry->SVMDirEntry.DModify.DMinute<<
         "\tDModify.DSecond = "<<(unsigned int)newEntry->SVMDirEntry.DModify.DSecond<<endl; */
        
        newEntry->SVMDirEntry.DAttributes = newEntry->DIR_Attr;
        newEntry->SVMDirEntry.DSize = newEntry->DIR_FileSize;
        //cerr <<"\nDAttributes = "<<(unsigned int)newEntry->SVMDirEntry.DAttributes<<"\tDSize = "<<(unsigned int)newEntry->SVMDirEntry.DSize<<endl;
        memcpy(newEntry->SVMDirEntry.DShortFileName, newEntry->DIR_Name, sizeof(newEntry->SVMDirEntry.DShortFileName));
        //cerr <<"DShortFileName = "<<newEntry->SVMDirEntry.DShortFileName<<endl;
        
        /*
         typedef struct{
         unsigned int DYear;
         unsigned char DMonth;
         unsigned char DDay;
         unsigned char DHour;
         unsigned char DMinute;
         unsigned char DSecond;
         unsigned char DHundredth;
         } SVMDateTime, *SVMDateTimeRef;
         
         typedef struct{
         char DLongFileName[VM_FILE_SYSTEM_MAX_PATH];
         char DShortFileName[VM_FILE_SYSTEM_SFN_SIZE];
         unsigned int DSize;
         unsigned char DAttributes;
         SVMDateTime DCreate;
         SVMDateTime DAccess;
         SVMDateTime DModify;
         } SVMDirectoryEntry, *SVMDirectoryEntryRef;
         
         */
        
        rootTable.push_back( newEntry);
        
        /*debug();
         cerr << " DIR_Attr = " << newEntry->DIR_Attr << endl;
         cerr << " DIR_FstClsHI = " << newEntry->DIR_FstClusHI << endl;
         cerr << " DIR_FstClsLO = " << newEntry->DIR_FstClusLO << endl;
         cerr << " DIR_FileSize = " << newEntry->DIR_FileSize << endl;
         cerr << " DIR_Name = " << newEntry->DIR_Name << endl; */
      }
      
    }
    
    cerr << " Finished Load All Root Entries" << endl;
  }

  
  
  void loadFatTable()
  {
    //Read in Entire Fat
    ReadSector(globalBPB->BPB_RsvdSecCnt);
    //cerr << " Loading Fat Table" << endl;
    
    //PrintCharFat();
    //PrintNumFat();
    
    //resetDebug();
    fatEntry *newFatEntry;
    cluster *newCluster;
    //debug();
    newFatEntry = new fatEntry;
    
    
    for (int j = 0; j<globalBPB->ClusterCount && (EndiantoInt(2,&fatData[j*2]) != 0) ; j++) //EndiantoInt(2, &fatData[j*2]) < 65528 ; j++)
    {
      newCluster = new cluster;
      
      newCluster->nextCluster = EndiantoInt(2,&fatData[j*2]);
      if (newCluster->nextCluster > 65528) newCluster->hasNextCluster = false;
      else newCluster->hasNextCluster = true;
      newCluster->startingSector = globalBPB->FirstDataSector + (j-2)*2;
      
      //if (newCluster->hasNextCluster) cerr << "Cluster " << j << " has next cluster" << newCluster->nextCluster << endl;
      //else cerr << "Cluster " << j << " has no next cluster" << endl;
      
      newFatEntry->clusterChain.push_back(newCluster);
    }
    
    fatTable.push_back(newFatEntry);
    
  }
  

  /*
   typedef struct{
    char DLongFileName[VM_FILE_SYSTEM_MAX_PATH];
    char DShortFileName[VM_FILE_SYSTEM_SFN_SIZE];
    unsigned int DSize;
    unsigned char DAttributes;
    SVMDateTime DCreate;
    SVMDateTime DAccess;
    SVMDateTime DModify;
  } SVMDirectoryEntry, *SVMDirectoryEntryRef;
  */

  
  
} // extern C
