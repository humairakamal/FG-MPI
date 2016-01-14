/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 *  (C) 2014 FG-MPI: Fine-Grain MPI.
 *      See copyright in top-level directory.
 */

#if defined(FINEGRAIN_MPI)

#include "threadlib.h"
#include "threadlib_internal.h"
#include "pmi.h" /* PMI_Get_size */
#include "mpiimpl.h"

/* fgmpi_sleepq_size defined in src/fgutil/src/threadlib.c */
int fgmpi_sleepq_size(void);

FGP_tuple_t *pid_to_fgps;          /* FG: pid_to_fgps is a 1-many mapping from the HWP pid
                                      to the FG worldranks in MPI_COMM_WORLD. */
RTWmap *worldcomm_rtw_map;
hshtbl *contextLeader_hshtbl;      /* This will remain a hashtable. */
struct coproclet_barrier_vars *worldcomm_barrier_vars;
struct MPIDI_VCRT * vcrt_world;    /* virtual connecton reference table for MPI_COMM_WORLD */
hshtbl *cidLookup_hshtbl;          /* FG: This is the new cid lookup hashtable that will be used with
                                    the new CID = <LID,LBI> algorithm. */
Coproclet_shared_vars_t * world_co_shared_vars;


static void* FG_Process_wrapper( void* wrapargs );
void FG_Spawn_threads(FG_WrapperProcessPtr_t WrapProcessPtr, FWraparg_t* FG_WrapArgs, int num_spawn, int *argc, char ***argv);
extern void (*sched_start_progress_thread)(void);
extern void (*sched_init_progress_thread)(void);
static int FGP_inits = 0;
static int numFGspawn = 0;
static fgmpi_thread_t** Fprog_spawn_ret = NULL;
FWraparg_t* FG_Wrapargs = NULL;

FGP_Init_State_t FGP_init_state = FGP_PRE_INIT;

void FG_Init(void)
{
    FGP_inits++;
    /* Adding an FGP barrier */
    while(FGP_inits < (numFGspawn+1))
    {
      runlist_init();  /* The number of times this is called is numfgps-1 */
    }
    return;
}



void FG_Finalize(void)
{
    int i;

    /* co_main waits for all coroutines to finish before exiting. */
    for(i=0; i<numFGspawn; i++)
    {
        thread_join(Fprog_spawn_ret[i], NULL);
    }

    if (progress_thread ) {
        if (!is_progress_thread_running())
        {
            sched_start_progress_thread();
        }
        thread_join(progress_thread, NULL);
    }

    if (Fprog_spawn_ret)
    {
        MPIU_Free(Fprog_spawn_ret);
    }
    if (FG_Wrapargs) {
        MPIU_Free(FG_Wrapargs);
    }

    return;
}


void FG_Spawn_threads(FG_WrapperProcessPtr_t WrapProcessPtr, FWraparg_t* FG_WrapArgs, int num_spawn, int *argc, char ***argv)
{
    int i;
    FG_WrapperProcessPtr_t FG_wrapperFPtr = WrapProcessPtr;

    numFGspawn = num_spawn;
    MALLOC(Fprog_spawn_ret, numFGspawn, fgmpi_thread_t*, fgmpi_thread_t**);

    /* creating coroutines of the function pointers */
    /* IMPORTANT NOTE: DO NOT REUSE THE ARGUMENTS PASSED TO EACH thread_spawn.
       EACH MUST HAVE ITS OWN SEPARATE ARGUMENTS ALLOCATED THOUGH MALLOC ABOVE. */
    for (i=0; i<num_spawn; i++)
    {
	  Fprog_spawn_ret[i] = thread_spawn("FG_PROG", (FG_WrapperProcessPtr_t)(FG_wrapperFPtr), (void*)(&FG_WrapArgs[i]));

    }

    /* Adding an FGP barrier */
    while(FGP_inits < numFGspawn)
        {
            runlist_init();
        }
    FGP_inits++; /* This is Main co */
    FGP_init_state = FGP_ALL_POST_INIT;

    sched_init_progress_thread();

    return;
}


void* FG_Process_wrapper( void* wrapargs )
{
    FWraparg_t * wargs = (FWraparg_t*)wrapargs;
    int *fargc = &(wargs->argc);
    char*** fargv = &(wargs->argv);
    FG_ProcessPtr_t FG_MPI_processPtr = wargs->FG_FPtr;
    (*FG_MPI_processPtr)(*fargc, *fargv);
    return (0);
}

int FGmpiexec( int *argc, char ***argv, LookupMapPtr_t lookupFuncPtr )
{
    int init_errno = 0;
    int i, numfgps=-1, fgstartrank=-1, numspawn;
    char *mymapstr = NULL;
    FG_MapPtr_t FG_MAP_FunctionPtr = NULL;
    FG_ProcessPtr_t FG_MPI_processPtr = NULL;
    FG_WrapperProcessPtr_t WrapProcessPtr = NULL;
    FG_ProcessPtr_t FG_MPI_Main_processPtr = NULL;

    if (NULL == lookupFuncPtr){
        printf("A valid lookup function is missing.... bailing out\n");
        exit(-1);
    }

    init_errno = MPI_Init(argc, argv);
    if (init_errno) goto fn_exit;

    MPIX_Get_collocated_size(&numfgps);
    assert(numfgps >= 1);
    numspawn = numfgps-1;
    MPIX_Get_collocated_startrank(&fgstartrank);
    assert(fgstartrank >= 0);

    mymapstr = getenv("FGMAP");
    FG_MAP_FunctionPtr = (FG_MapPtr_t)lookupFuncPtr(*argc, *argv, mymapstr);
    if (NULL == FG_MAP_FunctionPtr){
        printf("A valid map function is missing.... bailing out\n");
        exit(-1);
    }
    FG_MAP_FunctionPtr(*argc, *argv, MAP_INIT_ACTION);
    FG_MPI_Main_processPtr = FG_MAP_FunctionPtr(*argc, *argv, fgstartrank);

    if(numspawn > 0) {
        FG_Wrapargs = (FWraparg_t*)MPIU_Malloc(numspawn * sizeof(FWraparg_t));
        assert(FG_Wrapargs);

        /* mapping to functions */
        for (i=0; i< numspawn; i++) {
            FG_MPI_processPtr = FG_MAP_FunctionPtr(*argc, *argv, (i+1)+fgstartrank);
            FG_Wrapargs[i].argc = *argc;
            FG_Wrapargs[i].argv = *argv;
            FG_Wrapargs[i].FG_FPtr = FG_MPI_processPtr;
        }
        FG_MAP_FunctionPtr(*argc, *argv, MAP_FINALIZE_ACTION);

        WrapProcessPtr = &FG_Process_wrapper;
        FG_Spawn_threads(WrapProcessPtr, FG_Wrapargs, numspawn, argc, argv);
    }

    (*FG_MPI_Main_processPtr)(*argc, *argv);

 fn_exit:
    return (init_errno);
}


inline void* FG_MPIU_Malloc(size_t a, const char fcname[], int lineno){
    void * retval;
    retval = MPIU_Malloc((size_t)(a));
    if(retval){
        return retval;
    }
    else{
        printf("%s:_Line_%d Memory Allocation Failure!\n", fcname, lineno);
        assert(0);
    }
    return NULL;
}

inline void FG_yield(const char fcname[], int lineno){
    int numfgps;
    MPIX_Get_collocated_size(&numfgps);
    if( (numfgps > 1) && ((runlist_size() > 0) || (fgmpi_sleepq_size() > 0)) )
    { /* FG: -nfg X, where X is numfgps*/
        thread_yield();
    }
}

inline void MPIX_Yield(void){
    thread_yield();
}

void  mpix_yield_ (void){
     MPIX_Yield();
}

inline void MPIX_Usleep(unsigned long long utime)
{
    thread_usleep(utime);
}

void mpix_usleep_(unsigned long long utime)
{
     MPIX_Usleep(utime);
}

inline void FG_yield_on_event(scheduler_event yld_event, const char fcname[], int lineno){
    int numfgps;
    MPIX_Get_collocated_size(&numfgps);
    if( (numfgps > 1) && ((runlist_size() > 0) || (fgmpi_sleepq_size() > 0)) )
    { /* FG: -nfg X, where X is numfgps*/
        thread_yield_on_event(yld_event);
    }
}

inline void FG_notify_on_event(int worldrank, int action, const char fcname[], int lineno){
    scheduler_event notification;
    notification.worldrank = worldrank;
    notification.action_on_event = action;
    thread_notify_on_event(notification);
}

#define LESS -1
#define EQUAL 0
#define GREATER 1

static int FG_is_within(int rank, int start_fgrank, int numfgps)
{
    if( (rank >=start_fgrank) && (rank < (start_fgrank+numfgps)) ) /* found it */
        return EQUAL;
    else if(rank >= (start_fgrank+numfgps))
        return GREATER;
    else if(rank < start_fgrank)
        return LESS;
    else {
        MPL_internal_error_printf("Error: In FG_is_within(). This part of code should not be reached in file %s at line %d\n", __FILE__, __LINE__);
        MPID_Abort(NULL, MPI_SUCCESS, -1, NULL);
        MPL_exit(-1); /* If for some reason MPID_Abort returns, exit here. */
        return 911; /* silence compiler warning - use C11 _Noreturn some day */
    }
}


/* For binary search the fg_startrank in pid_to_fgps array must sorted.
 */
static int FGI_worldrank_to_pid(const int fgwrank, int *pid_ptr)
{
    int low, high, mid;
    int pid_size;
    PMI_Get_size(&pid_size); /* number of HWPs */

    low = 0;
    high = pid_size - 1;

    while (low <= high)
    {
        mid = (low + high) / 2;
        if (LESS == FG_is_within(fgwrank, pid_to_fgps[mid].fg_startrank, pid_to_fgps[mid].numfgps))
            high = mid - 1;
        else if (GREATER == FG_is_within(fgwrank, pid_to_fgps[mid].fg_startrank, pid_to_fgps[mid].numfgps))
            low = mid + 1;
        else
        {
              *pid_ptr = mid;
              MPIU_Assert((*pid_ptr) >=0);
              return MPI_SUCCESS;  /* found */
        }
    }
    return 1;  /* not found. This should not happen! */

}

/* Takes a FGworldrank as parameter returns its HWP rank (pid) for  MPI_COMM_WORLD only. */
int FG_worldrank_to_pid(int FG_worldrank)
{
    int pid, reterr;
    reterr = FGI_worldrank_to_pid(FG_worldrank, &pid);
    if(MPI_SUCCESS != reterr)
    {
        MPL_internal_error_printf("Error: No pid found for worldrank=%d. This part of code should not be reached in file %s at line %d\n", FG_worldrank, __FILE__, __LINE__);
        MPID_Abort(NULL, MPI_SUCCESS, -1, NULL);
        MPL_exit(-1); /* If for some reason MPID_Abort returns, exit here. */
    }
    return pid;
}


int RTWPmapFind(RTWmap* rtw_map, int lrank, int *worldrank_ptr, int *pid_ptr){ /* IN,IN,OUT,OUT */
    if ( lrank < 0 ) {
        MPL_internal_error_printf("Error: negative rank lookup. This part of code should not be reached in file %s at line %d\n", __FILE__, __LINE__);
        MPID_Abort(NULL, MPI_SUCCESS, -1, NULL);
        MPL_exit(-1);
    }
    if(rtw_map == worldcomm_rtw_map){
        *worldrank_ptr = lrank;
    }
    else{
        int ret = RTWmapFind(rtw_map, lrank, worldrank_ptr);
        MPIU_Assert(ret == MPI_SUCCESS);
    }
    if (NULL != pid_ptr){
        *pid_ptr = FG_worldrank_to_pid(*worldrank_ptr);
    }
    return (MPI_SUCCESS);
}

int FG_set_PROC_NULL(int *worldrank_ptr, int *pid_ptr) /* OUT,OUT */
{
    (*worldrank_ptr) = MPI_PROC_NULL;
    if (NULL != pid_ptr) {
        (*pid_ptr) = MPI_PROC_NULL;
    }
    return (MPI_SUCCESS);
}


/* Returns 1 if FGrank and comm->fgrank have same pid in the
   communicator comm. Otherwise, returns 0. */
int FG_is_within_same_HWP(int reqfgrank, MPID_Comm *comm, int *reqrankpid)
{
    int reqpid=-1, worldrank = -1, commfgrankpid=-2; /* Unequal initializers */
    MPIU_Assert( NULL != comm );
    MPIU_Assert( (reqfgrank > MPI_ANY_SOURCE) && (reqfgrank < comm->local_size) );
    MPIDI_Comm_get_pid_worldrank(comm, reqfgrank, &reqpid, &worldrank);
    if(NULL != reqrankpid){
        *reqrankpid = reqpid;
    }

    commfgrankpid = comm->p_rank; /* comm->p_rank is the HWP pid  _p_rank_ */
    if (commfgrankpid == reqpid)
        return (1);
    else
        return (0);
}


#endif /* #if defined (FINEGRAIN_MPI) */



