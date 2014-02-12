/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include "ocr-config.h"
#ifdef ENABLE_WORKER_HC

#include "debug.h"
#include "ocr-comp-platform.h"
#include "ocr-policy-domain.h"
#include "ocr-runtime-types.h"
#include "ocr-sysboot.h"
#include "ocr-types.h"
#include "ocr-worker.h"
#include "ocr-db.h"
#include "worker/hc/hc-worker.h"

#ifdef OCR_ENABLE_STATISTICS
#include "ocr-statistics.h"
#include "ocr-statistics-callbacks.h"
#endif

#define DEBUG_TYPE WORKER

/******************************************************/
/* OCR-HC WORKER                                      */
/******************************************************/

// Convenient to have an id to index workers in pools
static inline u64 getWorkerId(ocrWorker_t * worker) {
    ocrWorkerHc_t * hcWorker = (ocrWorkerHc_t *) worker;
    return hcWorker->id;
}

/**
 * The computation worker routine that asks work to the scheduler
 */
static void workerLoop(ocrWorker_t * worker) {
    ocrPolicyDomain_t *pd = worker->pd;
    ocrPolicyMsg_t msg;
    getCurrentEnv(NULL, NULL, NULL, &msg);
    while(worker->fcts.isRunning(worker)) {
        ocrFatGuid_t taskGuid = {.guid = NULL_GUID, .metaDataPtr = NULL};
        u32 count = 1;
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_COMM_TAKE
        msg.type = PD_MSG_COMM_TAKE | PD_MSG_REQUEST | PD_MSG_REQ_RESPONSE;
        PD_MSG_FIELD(guids) = &taskGuid;
        PD_MSG_FIELD(guidCount) = count;
        PD_MSG_FIELD(properties) = 0;
        PD_MSG_FIELD(type) = OCR_GUID_EDT;
        // TODO: In the future, potentially take more than one)
        if(pd->processMessage(pd, &msg, true) == 0) {
            // We got a response
            count = PD_MSG_FIELD(guidCount);
            if(count == 1) {
                ASSERT(taskGuid.guid != NULL_GUID && taskGuid.metaDataPtr != NULL);
                worker->curTask = (ocrTask_t*)taskGuid.metaDataPtr;
                u8 (*executeFunc)(ocrTask_t *) = (u8 (*)(ocrTask_t*))PD_MSG_FIELD(extra); // Execute is stored in extra
                executeFunc(worker->curTask);
                worker->curTask = NULL;
                // Destroy the work
#undef PD_TYPE
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_WORK_DESTROY
                msg.type = PD_MSG_WORK_DESTROY | PD_MSG_REQUEST;
                PD_MSG_FIELD(guid) = taskGuid;
                PD_MSG_FIELD(properties) = 0;
                // Ignore failures, we may be shutting down
                pd->processMessage(pd, &msg, false);
#undef PD_MSG
#undef PD_TYPE
            }
        }
    } /* End of while loop */
}

void destructWorkerHc(ocrWorker_t * base) {
    u64 i = 0;
    while(i < base->computeCount) {
        base->computes[i]->fcts.destruct(base->computes[i]);
        ++i;
    }
    runtimeChunkFree((u64)(base->computes), NULL);    
    runtimeChunkFree((u64)base, NULL);
}

/**
 * Builds an instance of a HC worker
 */
ocrWorker_t* newWorkerHc (ocrWorkerFactory_t * factory,
                          ocrParamList_t * perInstance) {
    ocrWorkerHc_t * worker = (ocrWorkerHc_t*)runtimeChunkAlloc(
        sizeof(ocrWorkerHc_t), NULL);
    ocrWorker_t * base = (ocrWorker_t *) worker;

    base->fguid.guid = UNINITIALIZED_GUID;
    base->fguid.metaDataPtr = base;
    base->pd = NULL;
    base->curTask = NULL;
    base->fcts = factory->workerFcts;
    
    worker->id = ((paramListWorkerHcInst_t*)perInstance)->workerId;
    worker->running = false;
    worker->secondStart = false;

    base->type = ((paramListWorkerHcInst_t*)perInstance)->workerType;
    ASSERT((worker->id && base->type == SLAVE_WORKERTYPE) ||
           (worker->id == 0 && base->type == MASTER_WORKERTYPE));
    
    base->fcts = factory->workerFcts;
    return base;
}

void hcBeginWorker(ocrWorker_t * base, ocrPolicyDomain_t * policy) {
    
    // Starts everybody, the first comp-platform has specific
    // code to represent the master thread.
    u64 computeCount = base->computeCount;
    ASSERT(computeCount == 1); 
    u64 i = 0;
    for(i = 0; i < computeCount; ++i) {
        base->computes[i]->fcts.begin(base->computes[i], policy, base->type);
#ifdef OCR_ENABLE_STATISTICS
        statsWORKER_START(policy, base->guid, base, base->computes[i]->guid, base->computes[i]);
#endif
    }

    if(base->type == MASTER_WORKERTYPE) {
        // For the master thread, we need to set the PD and worker
        // The other threads will set this when they start
        for(i = 0; i < computeCount; ++i) {
            base->computes[i]->fcts.setCurrentEnv(base->computes[i], policy, base);
        }
    }
}

void hcStartWorker(ocrWorker_t * base, ocrPolicyDomain_t * policy) {
    
    ocrWorkerHc_t * hcWorker = (ocrWorkerHc_t *) base;
    if(base->type == MASTER_WORKERTYPE) {
        if(!hcWorker->secondStart) {
            hcWorker->secondStart = true;
            return; // Don't start right away
        }
    }
    
    // Get a GUID
    guidify(policy, (u64)base, &(base->fguid), OCR_GUID_WORKER);
    base->pd = policy;
    
    ASSERT(base->type != MASTER_WORKERTYPE || hcWorker->secondStart);
    hcWorker->running = true;

    // Starts everybody, the first comp-platform has specific
    // code to represent the master thread.
    u64 computeCount = base->computeCount;
    // What the compute target will execute
    ASSERT(computeCount == 1); 
    u64 i = 0;
    for(i = 0; i < computeCount; ++i) {
        base->computes[i]->fcts.start(base->computes[i], policy, base);
#ifdef OCR_ENABLE_STATISTICS
        statsWORKER_START(policy, base->guid, base, base->computes[i]->guid, base->computes[i]);
#endif
    }
    // Otherwise, it is highly likely that we are shutting down
}

void* hcRunWorker(ocrWorker_t * worker) {
    if (worker->type != MASTER_WORKERTYPE) {
        // Set who we are
        ocrPolicyDomain_t *pd = worker->pd;
        u32 i;
        for(i = 0; i < worker->computeCount; ++i) {
            worker->computes[i]->fcts.setCurrentEnv(worker->computes[i], pd, worker);
        }
    } else {
        // This is all part of the mainEdt setup 
        // and should be executed by the "blessed" worker.
        void * packedUserArgv = userArgsGet();
        ocrEdt_t mainEdt = mainEdtGet();

        u64 totalLength = ((u64*) packedUserArgv)[0]; // already exclude this first arg
        // strip off the 'totalLength first argument'
        packedUserArgv = (void *) (((u64)packedUserArgv) + sizeof(u64)); // skip first totalLength argument
        ocrGuid_t dbGuid;
        void* dbPtr;
        ocrDbCreate(&dbGuid, &dbPtr, totalLength,
                    DB_PROP_NONE, NULL_GUID, NO_ALLOC);
        
        // copy packed args to DB
        hal_memCopy(dbPtr, packedUserArgv, totalLength, 0);

        // Prepare the mainEdt for scheduling
        ocrGuid_t edtTemplateGuid, edtGuid;
        ocrEdtTemplateCreate(&edtTemplateGuid, mainEdt, 0, 1);
        ocrEdtCreate(&edtGuid, edtTemplateGuid, EDT_PARAM_DEF, /* paramv = */ NULL,
                    /* depc = */ EDT_PARAM_DEF, /* depv = */ &dbGuid,
                    EDT_PROP_NONE, NULL_GUID, NULL);
    }
    
    DPRINTF(DEBUG_LVL_INFO, "Starting scheduler routine of worker %ld\n", getWorkerId(worker));
    workerLoop(worker);
    return NULL;
}

void hcFinishWorker(ocrWorker_t * base) {
    DPRINTF(DEBUG_LVL_INFO, "Finishing worker routine %ld\n", getWorkerId(base));
    ASSERT(base->computeCount == 1); 
    u64 i = 0;
    for(i = 0; i < base->computeCount; i++) {
        base->computes[i]->fcts.finish(base->computes[i]);
    }
}

void hcStopWorker(ocrWorker_t * base) {
    ocrWorkerHc_t * hcWorker = (ocrWorkerHc_t *) base;
    hcWorker->running = false;
    
    u64 computeCount = base->computeCount;
    u64 i = 0;
    // This code should be called by the master thread to join others
    for(i = 0; i < computeCount; i++) {
        base->computes[i]->fcts.stop(base->computes[i]);
#ifdef OCR_ENABLE_STATISTICS
        statsWORKER_STOP(base->pd, base->fguid.guid, base->fguid.metaDataPtr,
                         base->computes[i]->fguid.guid,
                         base->computes[i]->fguid.metaDataPtr);
#endif
    }
    DPRINTF(DEBUG_LVL_INFO, "Stopping worker %ld\n", getWorkerId(base));

    // Destroy the GUID
    ocrPolicyMsg_t msg;
    getCurrentEnv(NULL, NULL, NULL, &msg);
    
#define PD_MSG (&msg)
#define PD_TYPE PD_MSG_GUID_DESTROY
    msg.type = PD_MSG_GUID_DESTROY | PD_MSG_REQUEST;
    PD_MSG_FIELD(guid) = base->fguid;
    PD_MSG_FIELD(properties) = 0;
    // Ignore failure here, we are most likely shutting down
    base->pd->processMessage(base->pd, &msg, false);
#undef PD_MSG
#undef PD_TYPE
    base->fguid.guid = UNINITIALIZED_GUID;
}

bool hcIsRunningWorker(ocrWorker_t * base) {
    ocrWorkerHc_t * hcWorker = (ocrWorkerHc_t *) base;
    return hcWorker->running;
}

u8 hcSendMessage(ocrWorker_t *self, ocrLocation_t location, ocrPolicyMsg_t **msg) {
    ASSERT(self->computeCount == 1);
    return self->computes[0]->fcts.sendMessage(self->computes[0], location, msg);
}

u8 hcPollMessage(ocrWorker_t *self, ocrPolicyMsg_t **msg, u32 mask) {
    ASSERT(self->computeCount == 1);
    return self->computes[0]->fcts.pollMessage(self->computes[0], msg, mask);
}

u8 hcWaitMessage(ocrWorker_t *self, ocrLocation_t location, ocrPolicyMsg_t **msg) {
    ASSERT(self->computeCount == 1);
    return self->computes[0]->fcts.waitMessage(self->computes[0], msg);
}

/******************************************************/
/* OCR-HC WORKER FACTORY                              */
/******************************************************/

void destructWorkerFactoryHc(ocrWorkerFactory_t * factory) {
    runtimeChunkFree((u64)factory, NULL);
}

ocrWorkerFactory_t * newOcrWorkerFactoryHc(ocrParamList_t * perType) {
    ocrWorkerFactoryHc_t* derived = (ocrWorkerFactoryHc_t*)runtimeChunkAlloc(sizeof(ocrWorkerFactoryHc_t), (void *)1);
    ocrWorkerFactory_t* base = (ocrWorkerFactory_t*) derived;
    base->instantiate = newWorkerHc;
    base->destruct =  destructWorkerFactoryHc;
    base->workerFcts.destruct = destructWorkerHc;
    base->workerFcts.begin = hcBeginWorker;
    base->workerFcts.start = hcStartWorker;
    base->workerFcts.run = hcRunWorker;
    base->workerFcts.stop = hcStopWorker;
    base->workerFcts.finish = hcFinishWorker;
    base->workerFcts.isRunning = hcIsRunningWorker;
    base->workerFcts.sendMessage = FUNC_ADDR(u8 (*)(ocrWorker_t*, ocrLocation_t, ocrPolicyMsg_t**), hcSendMessage);
    base->workerFcts.pollMessage = FUNC_ADDR(u8 (*)(ocrWorker_t*, ocrPolicyMsg_t**, u32), hcPollMessage);
    base->workerFcts.waitMessage = FUNC_ADDR(u8 (*)(ocrWorker_t*, ocrPolicyMsg_t**), hcWaitMessage);
    return base;
}

#endif /* ENABLE_WORKER_HC */
