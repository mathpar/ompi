/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * Copyright (c) 2004 The Ohio State University.
 *                    All rights reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

/* #include <hh_common.h> */

/* Open MPI includes */
#include "ompi_config.h"
#include "include/constants.h"
#include "event/event.h"
#include "util/if.h"
#include "util/argv.h"
#include "util/output.h"
#include "mca/pml/pml.h"
#include "mca/bmi/bmi.h"
#include "mca/base/mca_base_param.h"
#include "mca/base/mca_base_module_exchange.h"
#include "mca/errmgr/errmgr.h"

/* IB bmi includes */
#include "bmi_ib.h"


mca_bmi_ib_component_t mca_bmi_ib_component = {
    {
        /* First, the mca_base_component_t struct containing meta information
           about the component itself */

        {
            /* Indicate that we are a pml v1.0.0 component (which also implies a
               specific MCA version) */

            MCA_BMI_BASE_VERSION_1_0_0,

            "ib", /* MCA component name */
            1,  /* MCA component major version */
            0,  /* MCA component minor version */
            0,  /* MCA component release version */
            mca_bmi_ib_component_open,  /* component open */
            mca_bmi_ib_component_close  /* component close */
        },

        /* Next the MCA v1.0.0 component meta data */

        {
            /* Whether the component is checkpointable or not */

            false
        },

        mca_bmi_ib_component_init,  
        mca_bmi_ib_component_control,
        mca_bmi_ib_component_progress,
    }
};


/*
 * utility routines for parameter registration
 */

static inline char* mca_bmi_ib_param_register_string(
        const char* param_name, 
        const char* default_value)
{
    char *param_value;
    int id = mca_base_param_register_string("bmi","ib",param_name,NULL,default_value);
    mca_base_param_lookup_string(id, &param_value);
    return param_value;
}

static inline int mca_bmi_ib_param_register_int(
        const char* param_name, 
        int default_value)
{
    int id = mca_base_param_register_int("bmi","ib",param_name,NULL,default_value);
    int param_value = default_value;
    mca_base_param_lookup_int(id,&param_value);
    return param_value;
}

/*
 *  Called by MCA framework to open the component, registers
 *  component parameters.
 */

int mca_bmi_ib_component_open(void)
{
    /* register component parameters */
    mca_bmi_ib_module.super.bmi_exclusivity =
        mca_bmi_ib_param_register_int ("exclusivity", 0);

    mca_bmi_ib_module.super.bmi_first_frag_size =
        mca_bmi_ib_param_register_int ("first_frag_size",
                (MCA_BMI_IB_FIRST_FRAG_SIZE
                 - sizeof(mca_bmi_base_header_t)));

    mca_bmi_ib_module.super.bmi_min_frag_size =
        mca_bmi_ib_param_register_int ("min_frag_size",
                (MCA_BMI_IB_FIRST_FRAG_SIZE 
                 - sizeof(mca_bmi_base_header_t)));

    mca_bmi_ib_module.super.bmi_max_frag_size =
        mca_bmi_ib_param_register_int ("max_frag_size", 2<<30);

    /* register IB component parameters */
    mca_bmi_ib_component.ib_free_list_num =
        mca_bmi_ib_param_register_int ("free_list_num", 8);
    mca_bmi_ib_component.ib_free_list_max =
        mca_bmi_ib_param_register_int ("free_list_max", 1024);
    mca_bmi_ib_component.ib_free_list_inc =
        mca_bmi_ib_param_register_int ("free_list_inc", 32);
    mca_bmi_ib_component.ib_mem_registry_hints_log_size = 
        mca_bmi_ib_param_register_int ("hints_log_size", 8);

    /* initialize global state */
    mca_bmi_ib_component.ib_num_bmis=0;
    mca_bmi_ib_component.ib_bmis=NULL;
    OBJ_CONSTRUCT(&mca_bmi_ib_component.ib_procs, ompi_list_t);
    OBJ_CONSTRUCT (&mca_bmi_ib_component.ib_recv_frags, ompi_free_list_t);

    return OMPI_SUCCESS;
}

/*
 * component cleanup - sanity checking of queue lengths
 */

int mca_bmi_ib_component_close(void)
{
    D_PRINT("");
    /* Stub */
    return OMPI_SUCCESS;
}

/*
 *  IB component initialization:
 *  (1) read interface list from kernel and compare against component parameters
 *      then create a BMI instance for selected interfaces
 *  (2) setup IB listen socket for incoming connection attempts
 *  (3) register BMI parameters with the MCA
 */
mca_bmi_base_module_t** mca_bmi_ib_component_init(int *num_bmi_modules, 
                                                  bool enable_progress_threads,
                                                  bool enable_mpi_threads)
{
    VAPI_ret_t vapi_ret;
    VAPI_hca_id_t* hca_ids;
    mca_bmi_base_module_t** bmis;
    int i, ret;

    /* initialization */
    *num_bmi_modules = 0;

    /* query the list of available hcas */
    vapi_ret=EVAPI_list_hcas(0, &(mca_bmi_ib_component.ib_num_bmis), NULL);
    if( VAPI_EAGAIN != vapi_ret || 0 == mca_bmi_ib_component.ib_num_bmis ) {
        ompi_output(0,"Warning: no IB HCAs found\n");
        return NULL;
    }

    hca_ids = (VAPI_hca_id_t*) malloc(mca_bmi_ib_component.ib_num_bmis * sizeof(VAPI_hca_id_t));
    if(NULL == hca_ids) {
        ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
        return NULL;
    }
    vapi_ret=EVAPI_list_hcas(mca_bmi_ib_component.ib_num_bmis, &mca_bmi_ib_component.ib_num_bmis, hca_ids);
    if( VAPI_OK != vapi_ret ) {
        ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
        return NULL;
    }
                                                                                                                      
    /* Allocate space for bmi modules */
    mca_bmi_ib_component.ib_bmis = (mca_bmi_ib_module_t*) malloc(sizeof(mca_bmi_ib_module_t) * 
            mca_bmi_ib_component.ib_num_bmis);
    if(NULL == mca_bmi_ib_component.ib_bmis) {
        ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
        return NULL;
    }
    bmis = (struct mca_bmi_base_module_t**) 
        malloc(mca_bmi_ib_component.ib_num_bmis * sizeof(struct mca_bmi_ib_module_t*));
    if(NULL == bmis) {
        ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
        return NULL;
    }

    /* Initialize pool of receive fragments */
    ompi_free_list_init (&(mca_bmi_ib_component.ib_recv_frags),
            sizeof (mca_bmi_ib_recv_frag_t),
            OBJ_CLASS (mca_bmi_ib_recv_frag_t),
            mca_bmi_ib_component.ib_free_list_num,
            mca_bmi_ib_component.ib_free_list_max,
            mca_bmi_ib_component.ib_free_list_inc, NULL);

    /* Initialize each module */
    for(i = 0; i < mca_bmi_ib_component.ib_num_bmis; i++) {
        mca_bmi_ib_module_t* ib_bmi = &mca_bmi_ib_component.ib_bmis[i];

        /* Initialize the modules function pointers */
        memcpy(ib_bmi, &mca_bmi_ib_module, sizeof(mca_bmi_ib_module));

        /* Initialize module state */
        OBJ_CONSTRUCT(&ib_bmi->send_free, ompi_free_list_t);
        OBJ_CONSTRUCT(&ib_bmi->repost, ompi_list_t);

        ompi_free_list_init(&ib_bmi->send_free,
                sizeof(mca_bmi_ib_send_frag_t),
                OBJ_CLASS(mca_bmi_ib_send_frag_t),
                mca_bmi_ib_component.ib_free_list_num,
                mca_bmi_ib_component.ib_free_list_max,
                mca_bmi_ib_component.ib_free_list_inc,
                NULL);

      
        memcpy(ib_bmi->hca_id, hca_ids[i], sizeof(ib_bmi->hca_id));
        if(mca_bmi_ib_module_init(ib_bmi) != OMPI_SUCCESS) {
            free(hca_ids);
            return NULL;
        }

        /* Initialize the send descriptors */
        if(mca_bmi_ib_send_frag_register(ib_bmi) != OMPI_SUCCESS) {
            free(hca_ids);
            return NULL;
        }
        bmis[i] = &ib_bmi->super;
    }

    /* Post OOB receive to support dynamic connection setup */
    mca_bmi_ib_post_recv();

    *num_bmi_modules = mca_bmi_ib_component.ib_num_bmis;
    free(hca_ids);
    return bmis;
}

/*
 *  IB component control
 */

int mca_bmi_ib_component_control(int param, void* value, size_t size)
{
    return OMPI_SUCCESS;
}


/*
 *  IB component progress.
 */

#define MCA_BMI_IB_DRAIN_NETWORK(nic, cq_hndl, comp_type, comp_addr) \
{ \
    VAPI_ret_t ret; \
    VAPI_wc_desc_t comp; \
 \
    ret = VAPI_poll_cq(nic, cq_hndl, &comp); \
    if(VAPI_OK == ret) { \
        if(comp.status != VAPI_SUCCESS) { \
            ompi_output(0, "Got error : %s, Vendor code : %d Frag : %p", \
                    VAPI_wc_status_sym(comp.status), \
                    comp.vendor_err_syndrome, comp.id);  \
            *comp_type = IB_COMP_ERROR; \
            *comp_addr = NULL; \
        } else { \
            if(VAPI_CQE_SQ_SEND_DATA == comp.opcode) { \
                *comp_type = IB_COMP_SEND; \
                *comp_addr = (void*) (unsigned long) comp.id; \
            } else if(VAPI_CQE_RQ_SEND_DATA == comp.opcode) { \
                *comp_type = IB_COMP_RECV; \
                *comp_addr = (void*) (unsigned long) comp.id; \
            } else if(VAPI_CQE_SQ_RDMA_WRITE == comp.opcode) { \
                *comp_type = IB_COMP_RDMA_W; \
                *comp_addr = (void*) (unsigned long) comp.id; \
            } else { \
                ompi_output(0, "VAPI_poll_cq: returned unknown opcode : %d\n", \
                        comp.opcode); \
                *comp_type = IB_COMP_ERROR; \
                *comp_addr = NULL; \
            } \
        } \
    } else { \
        /* No completions from the network */ \
        *comp_type = IB_COMP_NOTHING; \
        *comp_addr = NULL; \
    } \
}


int mca_bmi_ib_component_progress(mca_bmi_tstamp_t tstamp)
{
    int i;
    int count = 0;

    /* Poll for completions */
    for(i = 0; i < mca_bmi_ib_component.ib_num_bmis; i++) {
        mca_bmi_ib_module_t* ib_bmi = &mca_bmi_ib_component.ib_bmis[i];
        int comp_type = IB_COMP_NOTHING;
        void* comp_addr;
        
        MCA_BMI_IB_DRAIN_NETWORK(ib_bmi->nic, ib_bmi->cq_hndl, &comp_type, &comp_addr);

        /* Handle n/w completions */
        switch(comp_type) {
            case IB_COMP_SEND :

                /* Process a completed send */
                mca_bmi_ib_send_frag_send_complete(ib_bmi, (mca_bmi_ib_send_frag_t*)comp_addr);
                count++;
                break;

            case IB_COMP_RECV :

                /* Process incoming receives */
                mca_bmi_ib_process_recv(ib_bmi, comp_addr);
                /* Re post recv buffers */
                if(ompi_list_get_size(&ib_bmi->repost) <= 1) {
                    ompi_list_append(&ib_bmi->repost, (ompi_list_item_t*)comp_addr);
                } else {
                    ompi_list_item_t* item;
                    while(NULL != (item = ompi_list_remove_first(&ib_bmi->repost))) {
                         mca_bmi_ib_buffer_repost(ib_bmi->nic, item);
                    }
                    mca_bmi_ib_buffer_repost(ib_bmi->nic, comp_addr);
                }
                count++;
                break;

            case IB_COMP_RDMA_W :

                ompi_output(0, "%s:%d RDMA not implemented\n", __FILE__,__LINE__);
                count++;
                break;

            case IB_COMP_NOTHING:
                break;
            default:
                ompi_output(0, "Errorneous network completion");
                break;
        }
    }
    return count;
}

