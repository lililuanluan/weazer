

#include <assert.h>
#include <pthread.h>

typedef struct ObjType {
    int field;
} ObjType;

void ObjInit(ObjType* obj) {
    obj->field = 0;
}
void ObjOperation(ObjType* obj) {
    obj->field++;
}
void ObjCheck(ObjType* obj) {
    assert(obj->field == 1);
}

#define MyObjTypeDef ObjType*		// mimic template

#include "WorkStealQueue.h"

#define INITQSIZE 2  // must be power of 2
int nStealers = 1;
int nItems = 4;
int nStealAttempts = 2;

// Argument parsing
int args(int argc, char** argv) {
    if (argc > 1) {
        int arg1 = atoi(argv[1]);
        if (arg1 > 0) {
            nStealers = arg1;
        }
    }
    if (argc > 2) {
        int arg2 = atoi(argv[2]);
        if (arg2 > 0) {
            nItems = arg2;
        }
    }
    if (argc > 3) {
        int arg3 = atoi(argv[3]);
        if (arg3 > 0) {
            nStealAttempts = arg3;
        }
    }
    printf("\nWorkStealQueue Test: %d stealers, %d items, and %d stealAttempts\n", nStealers, nItems, nStealAttempts);
    return 1;
}

void* Stealer(void* param) {
    WorkStealQueue* q = (WorkStealQueue*)param;
    ObjType* r;

    for (int i = 0; i < nStealAttempts; i++) {
        if (Steal(q, (MyObjTypeDef*)&r)) {
            ObjOperation(r);
        }
    }
    return NULL;
}

int main(int argc, char** argv) {
    args(argc, argv);

    // Thread handles and item objects
    pthread_t* handles = (pthread_t*)malloc(nStealers * sizeof(pthread_t));
    ObjType* items = (ObjType*)malloc(nItems * sizeof(ObjType));

    // Initialize queue
    WorkStealQueue q;
    init_queue(&q, INITQSIZE);

    // Initialize items
    for (int i = 0; i < nItems; i++) {
        ObjInit(&items[i]);
    }

    // Create stealer threads
    for (int i = 0; i < nStealers; i++) {
        pthread_create(&handles[i], NULL, Stealer, &q);
    }

    // Push and pop items from the queue
    for (int i = 0; i < nItems / 2; i++) {
        Push(&q, &items[2 * i]);
        Push(&q, &items[2 * i + 1]);

        ObjType* r;
        if (Pop(&q, (MyObjTypeDef*)&r)) {
            ObjOperation(r);
        }
    }

    for (int i = 0; i < nItems / 2; i++) {
        ObjType* r;
        if (Pop(&q, (MyObjTypeDef*)&r)) {
            ObjOperation(r);
        }
    }

    // Wait for threads to finish
    for (int i = 0; i < nStealers; i++) {
        pthread_join(handles[i], NULL);
    }

    // Check results
    for (int i = 0; i < nItems; i++) {
        ObjCheck(&items[i]);
    }

    // Free memory
    free(items);
    free(handles);

    return 0;
}