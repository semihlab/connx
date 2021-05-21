#include <inttypes.h>
#include <math.h> // sin, cos, ...
#include <stdio.h>
#include <stdlib.h> // strtol
#include <string.h> // memcpy
#include "tensor.h"
#include "accel.h"
#include "hal.h"

uint32_t connx_DataType_size(connx_DataType dtype) {
    switch(dtype) {
        case UINT8:
        case INT8:
        case BOOL:
            return 1;
        case UINT16:
        case INT16:
        case FLOAT16:
            return 2;
        case UINT32:
        case INT32:
        case FLOAT32:
            return 4;
        case UINT64:
        case INT64:
        case FLOAT64:
        case COMPLEX64:
            return 8;
        case COMPLEX128:
            return 16;
        case STRING:
            return sizeof(uintptr_t);
        case UNDEFINED:
        default:
            return 0;
    }
}

// Iterator
#define ITER_NDIM(iter) (iter)
#define ITER_START(iter) (iter + 1)
#define ITER_STOP(iter) (iter + 1 + iter[0])
#define ITER_STEP(iter) (iter + 1 + iter[0] * 2)
#define ITER_INDEX(iter) (iter + 1 + iter[0] * 3)

void connx_Iterator_init(int32_t* iterator, int32_t ndim, int32_t* start, int32_t* stop, int32_t* step) {
    *ITER_NDIM(iterator) = ndim;
    memcpy(ITER_START(iterator), start, sizeof(int32_t) * ndim);
    memcpy(ITER_STOP(iterator), stop, sizeof(int32_t) * ndim);
    memcpy(ITER_STEP(iterator), step, sizeof(int32_t) * ndim);
    memcpy(ITER_INDEX(iterator), start, sizeof(int32_t) * ndim);
    ITER_INDEX(iterator)[ndim - 1] -= step[ndim - 1];
}

bool connx_Iterator_next(int32_t* iterator) {
    int32_t ndim = *ITER_NDIM(iterator);
    int32_t* start = ITER_START(iterator);
    int32_t* stop = ITER_STOP(iterator);
    int32_t* step = ITER_STEP(iterator);
    int32_t* index = ITER_INDEX(iterator);

    // Go next step
    for(int32_t i = ndim - 1; i >= 0; i--) {
        index[i] += step[i];
        if(index[i] < stop[i])
            return true;
        else
            index[i] = start[i];
    }

    // Return to just before start
    memcpy(index, start, sizeof(int32_t) * ndim);
    index[ndim - 1] -= step[ndim - 1];

    return false;
}

int32_t connx_Iterator_ndim(int32_t* iterator) {
    return *ITER_NDIM(iterator);
}

int32_t* connx_Iterator_start(int32_t* iterator) {
    return ITER_START(iterator);
}

int32_t* connx_Iterator_stop(int32_t* iterator) {
    return ITER_STOP(iterator);
}

int32_t* connx_Iterator_step(int32_t* iterator) {
    return ITER_STEP(iterator);
}

int32_t* connx_Iterator_index(int32_t* iterator) {
    return ITER_INDEX(iterator);
}

int32_t connx_Iterator_offset(int32_t* iterator, int32_t* shape) {
    int32_t ndim = *ITER_NDIM(iterator);
    int32_t* index = ITER_INDEX(iterator);

    int32_t offset = 0;
    int32_t unit = 1;

    for(int32_t i = ndim - 1; i >= 0; i--) {
        offset += unit * index[i];
        unit *= shape[i];
    }

    return offset;
}

int32_t connx_Iterator_size(connx_Tensor* tensor) {
    return 1 + tensor->ndim * 4;
}

void connx_Iterator_dump(int32_t* iterator) {
    int32_t ndim = *ITER_NDIM(iterator);
    int32_t* start = ITER_START(iterator);
    int32_t* stop = ITER_STOP(iterator);
    int32_t* step = ITER_STEP(iterator);
    int32_t* index = ITER_INDEX(iterator);

#ifdef __linux__
    for(int32_t i = 0; i < ndim; i++)
        printf("%d ", index[i]);
    printf("/ ");
    for(int32_t i = 0; i < ndim; i++)
        printf("%d ", start[i]);
    printf("/ ");
    for(int32_t i = 0; i < ndim; i++)
        printf("%d ", stop[i]);
    printf("/ ");
    for(int32_t i = 0; i < ndim; i++)
        printf("%d ", step[i]);
    printf("\n");
#endif /* __linux__ */
}

/**
 * Tensor payload: [connx_Tensor] [shape] [buffer]
 * All the elements are aligned by CONNX_ALIGNMENT
 */
connx_Tensor* connx_Tensor_alloc(connx_DataType dtype, int32_t ndim, int32_t* shape) {
    uint32_t header_size = CONNX_ALIGN(sizeof(connx_Tensor));
    uint32_t dim_size = CONNX_ALIGN(sizeof(int32_t) * ndim);
    int32_t total = connx_Int32_product(ndim, shape);
    uint32_t data_size = connx_DataType_size(dtype) * total;
    uint32_t buffer_size = CONNX_ALIGN(data_size);

    void* ptr = connx_alloc(header_size + dim_size + buffer_size);
    if(ptr == NULL) {
        return NULL;
    }

    connx_Tensor* tensor = ptr;
    tensor->dtype = dtype;
    tensor->ndim = ndim;
    tensor->shape = ptr + header_size;
    memcpy(tensor->shape, shape, sizeof(int32_t) * ndim);
    tensor->buffer = ptr + header_size + dim_size;
    tensor->size = data_size;
    tensor->parent = NULL;
    tensor->ref_count = 1;
    connx_Lock_init(&tensor->lock);

    return tensor;
}

connx_Tensor* connx_Tensor_alloc_like(connx_Tensor* tensor) {
    return connx_Tensor_alloc(tensor->dtype, tensor->ndim, tensor->shape);
}

#define next_token(token)                       \
    ({                                          \
        char* start = token;                    \
        while(*token != '_' && *token != '.') { \
            token++;                            \
        }                                       \
        *token++ = '\0';                        \
        start;                                  \
    })

#define next_integer(token)               \
    ({                                    \
        char* number = next_token(token); \
        if(number == NULL)                \
            return NULL;                  \
        strtol(number, NULL, 0);          \
    })

connx_Tensor* connx_Tensor_load(const char* path) {
    int len = strlen(path);
    char path2[len + 1];
    memcpy(path2, path, len + 1);

    // Get basename
    char* token = path2 + len - 1;
    while(*token != '/')
        token--;
    token++;

    // Parse name
    next_token(token); // drop name
    int32_t data_type = next_integer(token);
    int32_t ndim = next_integer(token);
    int32_t shape[ndim];

    for(int32_t i = 0; i < ndim; i++) {
        shape[i] = next_integer(token);
    }

    // Create tensor
    int32_t total = connx_Int32_product(ndim, shape);
    int32_t data_size = connx_DataType_size(data_type);

    connx_Tensor* tensor = connx_Tensor_alloc(data_type, ndim, shape);
    if(tensor == NULL) {
        return NULL;
    }

    // Load data
    void* buf = connx_load(path);
    if(buf == NULL) {
        connx_error("Cannot load model data from path: %s\n", path);
        return NULL;
    }

    // Copy data
    memcpy(tensor->buffer, buf, total * data_size);
    connx_unload(buf);

    return tensor;
}

connx_Tensor* connx_Tensor_copy(connx_Tensor* tensor) {
    connx_Tensor* tensor2 = connx_Tensor_alloc_like(tensor);
    if(tensor2 == NULL)
        return NULL;

    int32_t total = connx_Int32_product(tensor->ndim, tensor->shape);
    int32_t data_size = connx_DataType_size(tensor->dtype);
    memcpy(tensor2->buffer, tensor->buffer, total * data_size);

    return tensor2;
}

connx_Tensor* connx_Tensor_reshape(connx_Tensor* tensor, int32_t ndim, int32_t* shape) {
    uint32_t header_size = CONNX_ALIGN(sizeof(connx_Tensor));
    uint32_t dim_size = CONNX_ALIGN(sizeof(int32_t) * ndim);

    void* ptr = connx_alloc(header_size + dim_size);
    if(ptr == NULL) {
        return NULL;
    }

    connx_Tensor* tensor2 = ptr;
    tensor2->dtype = tensor->dtype;
    tensor2->ndim = ndim;
    tensor2->shape = ptr + header_size;
    memcpy(tensor2->shape, shape, sizeof(int32_t) * ndim);
    tensor2->buffer = tensor->buffer;
    tensor2->size = tensor->size;
    tensor2->parent = tensor;
    tensor2->ref_count = 1;
    connx_Lock_init(&tensor2->lock);

    connx_Tensor_ref(tensor); // reshaped tensor references parent tensor

    return tensor2;
}

void connx_Tensor_ref(connx_Tensor* tensor) {
    connx_Lock_lock(&tensor->lock);

    tensor->ref_count++;

    connx_Lock_unlock(&tensor->lock);
}

void connx_Tensor_unref(connx_Tensor* tensor) {
    connx_Lock_lock(&tensor->lock);

    if(--tensor->ref_count <= 0) {
        connx_Tensor* parent = tensor->parent;

        connx_Lock_unlock(&tensor->lock);
        connx_Lock_destroy(&tensor->lock);

        connx_free(tensor);

        // Unref parent
        if(parent != NULL) {
            connx_Tensor_unref(parent);
        }

        return;
    }

    connx_Lock_unlock(&tensor->lock);
}

int connx_Tensor_get(connx_Tensor* tensor, int32_t* iterator, void* data) {
    int32_t offset = connx_Iterator_offset(iterator, tensor->shape);
    uint32_t data_size = connx_DataType_size(tensor->dtype);
    memcpy(data, tensor->buffer + offset * data_size, data_size);

    return OK;
}

int connx_Tensor_set(connx_Tensor* tensor, int32_t* iterator, void* data) {
    int32_t offset = connx_Iterator_offset(iterator, tensor->shape);
    uint32_t data_size = connx_DataType_size(tensor->dtype);
    memcpy(tensor->buffer + offset * data_size, data, data_size);

    return OK;
}

void connx_Tensor_dump(connx_Tensor* tensor) {
    int32_t unit = -1;
    int32_t unit2 = -1;

    if(tensor->ndim == 1) {
        unit = 8;
    } else if(tensor->ndim >= 1) {
        unit = tensor->shape[tensor->ndim - 1];

        if(tensor->ndim >= 2) {
            unit2 = unit * tensor->shape[tensor->ndim - 2];
        }
    }

    // New line by matrix
#define NEWLINE()              \
    if((i + 1) % unit == 0)    \
        fprintf(stderr, "\n"); \
                               \
    if((i + 1) % unit2 == 0)   \
        fprintf(stderr, "\n");


    fprintf(stderr, "tensor < ");
    for(int32_t i = 0; i < tensor->ndim; i++) {
        fprintf(stderr, "%u ", tensor->shape[i]);
    }

    int32_t total = connx_Int32_product(tensor->ndim, tensor->shape);
    fprintf(stderr, "> = %u\n", total);

    switch(tensor->dtype) {
        case UINT8: {
            uint8_t* array = tensor->buffer;
            for(int32_t i = 0; i < total; i++) {
                fprintf(stderr, "%" PRIu8 " ", array[i]);
                NEWLINE()
            }
            fprintf(stderr, "\n");
            break;
        }
        case INT8: {
            int8_t* array = tensor->buffer;
            for(int32_t i = 0; i < total; i++) {
                fprintf(stderr, "%" PRId8 " ", array[i]);
                NEWLINE()
            }
            fprintf(stderr, "\n");
            break;
        }
        case UINT16: {
            uint16_t* array = tensor->buffer;
            for(int32_t i = 0; i < total; i++) {
                fprintf(stderr, "%" PRIu16 " ", array[i]);
                NEWLINE()
            }
            fprintf(stderr, "\n");
            break;
        }
        case INT16: {
            int16_t* array = tensor->buffer;
            for(int32_t i = 0; i < total; i++) {
                fprintf(stderr, "%" PRId16 " ", array[i]);
                NEWLINE()
            }
            fprintf(stderr, "\n");
            break;
        }
        case UINT32: {
            uint32_t* array = tensor->buffer;
            for(int32_t i = 0; i < total; i++) {
                fprintf(stderr, "%" PRIu32 " ", array[i]);
                NEWLINE()
            }
            fprintf(stderr, "\n");
            break;
        }
        case INT32: {
            int32_t* array = tensor->buffer;
            for(int32_t i = 0; i < total; i++) {
                fprintf(stderr, "%" PRId32 " ", array[i]);
                NEWLINE()
            }
            fprintf(stderr, "\n");
            break;
        }
        case UINT64: {
            uint64_t* array = tensor->buffer;
            for(int32_t i = 0; i < total; i++) {
                fprintf(stderr, "%" PRIu64 " ", array[i]);
                NEWLINE()
            }
            fprintf(stderr, "\n");
            break;
        }
        case INT64: {
            int64_t* array = tensor->buffer;
            for(int32_t i = 0; i < total; i++) {
                fprintf(stderr, "%" PRId64 " ", array[i]);
                NEWLINE()
            }
            fprintf(stderr, "\n");
            break;
        }
        case FLOAT16: {
            uint16_t* array = tensor->buffer;
            for(int32_t i = 0; i < total; i++) {
                fprintf(stderr, "%" PRIu16 " ", array[i]);
                NEWLINE()
            }
            fprintf(stderr, "\n");
            break;
        }
        case FLOAT32: {
            float32_t* array = tensor->buffer;
            for(int32_t i = 0; i < total; i++) {
                fprintf(stderr, "%f ", array[i]);
                NEWLINE()
            }
            fprintf(stderr, "\n");
            break;
        }
        case FLOAT64: {
            float64_t* array = tensor->buffer;
            for(int32_t i = 0; i < total; i++) {
                fprintf(stderr, "%f ", array[i]);
                NEWLINE()
            }
            fprintf(stderr, "\n");
            break;
        }
        case STRING:
            fprintf(stderr, "Not implemented yet\n");
            break;
        case BOOL: {
            bool* array = tensor->buffer;
            for(int32_t i = 0; i < total; i++) {
                fprintf(stderr, "%s ", array[i] ? "true" : "false");
                NEWLINE()
            }
            fprintf(stderr, "\n");
            break;
        }
        case COMPLEX64:
        case COMPLEX128:
        case UNDEFINED:
        default:
            fprintf(stderr, "Not implemented yet\n");
    }
}
