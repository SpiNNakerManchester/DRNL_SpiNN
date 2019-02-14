/*
 ============================================================================
 Name        : SpiNNakEar_DRNL.c
 Author      : Robert James
 Version     : 1.0
 Description : Dual Resonance Non-Linear filterbank cochlea model for use in SpiNNakEar system
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdfix.h>
#include "DRNL_SpiNN.h"
#include "spin1_api.h"
#include "math.h"
#include "complex.h"
#include "random.h"
#include "stdfix-exp.h"
#include "log.h"
#include <data_specification.h>
#include <profiler.h>
#include <profile_tags.h>
#include <simulation.h>
#include <debug.h>

//#define PROFILE

//=========GLOBAL VARIABLES============//
REAL Fs,dt,max_rate;
uint coreID;
uint chipID;
uint test_DMA;
uint seg_index;
uint cbuff_index;
uint cbuff_numseg;
uint read_switch;
uint write_switch;
uint processing;
uint index_x;
uint index_y;
uint MC_seg_idx;
uint_float_union MC_union;
uint ack_rx=0;
uint moc_spike_count=0;
uint ms_counter=0;
uint moc_buffer_index = 0;

REAL cf,nlin_b0,nlin_b1,nlin_b2,nlin_a1,nlin_a2,
       lin_b0,lin_b1,lin_b2,lin_a1,lin_a2,lin_gain,
       a,ctBM,dispThresh,recip_ctBM,MOC,MOCnow1,
       MOCnow2,MOCnow3,MOCdec1,MOCdec2,MOCdec3,
       MOCfactor1,MOCfactor2,MOCfactor3,MOCspikeCount;


accum c;

REAL lin_x1;
REAL lin_y1[2],lin_y2[2];

REAL nlin_x1a;
REAL nlin_y1a[2],nlin_y2a[2];
REAL nlin_x1b;
REAL nlin_y1b[2],nlin_y2b[2];
REAL MOCtau[3],MOCtauweights[3];

int start_count_process;
int end_count_process;
int start_count_read;
int end_count_read;
int start_count_write;
int end_count_write;

uint sync_count=0;

float *dtcm_buffer_a;
float *dtcm_buffer_b;
REAL *dtcm_buffer_x;
REAL *dtcm_buffer_y;

REAL *sdramout_buffer;

//MOC count buffer
uint *moc_count_buffer;

//data spec regions
typedef enum regions {
    SYSTEM,
    PARAMS,
    RECORDING,
    PROFILER}regions;
// The parameters to be read from memory
enum params {
    DATA_SIZE = 0,
    OMECOREID,
    COREID,
    OMEAPPID,
    OME_KEY,
    KEY,
    NUM_IHCAN,
    CF,
    DELAY,
    FS,
    OME_DATA_KEY,
    MOC_CONN_LUT
};

// The size of the remaining data to be sent
uint data_size;
// the core ID given by the placement software
uint placement_coreID;
uint ome_coreID;
uint ome_appID;
uint ome_key;
uint key;
uint mask;
uint num_ihcans;
uint drnl_cf;
uint delay;
uint sampling_frequency;
uint ome_data_key;
uint n_mocs;
uint n_conn_lut_words;
uint *moc_conn_lut;
static key_mask_table_entry *key_mask_table;
static last_neuron_info_t last_neuron_info;

uint *moc_conn_lut_address;
uint n_seg_per_ms;

//application initialisation
void app_init(void)
{
	seg_index=0;
	cbuff_index=0;
	cbuff_numseg=3;
	read_switch=0;
	write_switch=0;

    //obtain data spec
	address_t data_address = data_specification_get_data_address();
    address_t params = data_specification_get_region(0, data_address);

	// Get the size of the data in words
    data_size = params[DATA_SIZE];//not used

    //obtain ome core ID from the host placement perspective
    ome_coreID = params[OMECOREID];
    //obtain this core ID from the host placement perspective
    placement_coreID = params[COREID];

    //obtain ome application ID from the host placement perspective
    ome_appID = params[OMEAPPID];
    //key for synchronisation messages to be sent back to parent OME
    ome_key=params[OME_KEY];
    log_info("omekey:%d",ome_key);
    //key for synchronisation messages to be sent to child IHC/ANs
    key=params[KEY];
    log_info("key:%d",key);

    //get the mask needed to extract comms protocol from MC keys
    mask = 3;
    //number of child IHC/ANs
    num_ihcans=params[NUM_IHCAN];
    //DRNL bandpass center frequency
    drnl_cf=params[CF];
    //transmission delay (not used)
    delay = params[DELAY];

    log_info("CF=%d\n",drnl_cf);
    //Get sampling frequency
    sampling_frequency = params[FS];
    Fs= (REAL)sampling_frequency;
	dt=(1.0/Fs);
	//calculate how many segments approx 1ms is
	n_seg_per_ms = (float)MOC_DELAY_MS/(SEGSIZE*(1000.*dt));
    log_info("n_seg_per_ms=%d\n",n_seg_per_ms);

	ome_data_key = params[OME_DATA_KEY];

	moc_conn_lut_address = &params[MOC_CONN_LUT];
	n_mocs = moc_conn_lut_address[0];
	io_printf(IO_BUF,"n_mocs=%d\n",n_mocs);
    n_conn_lut_words = moc_conn_lut_address[1];

    // Allocate buffers
    uint n_key_mask_table_bytes = n_mocs * sizeof(key_mask_table_entry);
    key_mask_table = (key_mask_table_entry *)spin1_malloc(n_key_mask_table_bytes);

    uint n_conn_lut_bytes = n_conn_lut_words * 4;
    moc_conn_lut = (uint *)spin1_malloc(n_conn_lut_bytes);

    spin1_memcpy(moc_conn_lut, &(moc_conn_lut_address[2]),
        n_conn_lut_bytes);

    spin1_memcpy(key_mask_table, &(moc_conn_lut_address[2+n_conn_lut_words]),
        n_key_mask_table_bytes);

	//output results buffer (shared with child IHCANs)
	//hack for smaller SDRAM intermediate circular buffers
	data_size=cbuff_numseg*SEGSIZE;

	sdramout_buffer = (REAL *) sark_xalloc (sv->sdram_heap,
					 data_size * sizeof(REAL),
					 placement_coreID,
					 ALLOC_LOCK);

	//DTCM input buffers
	dtcm_buffer_a = (float *) sark_alloc (SEGSIZE, sizeof(float));
	dtcm_buffer_b = (float *) sark_alloc (SEGSIZE, sizeof(float));
	//DTCM output buffers
	dtcm_buffer_x = (REAL *) sark_alloc (SEGSIZE, sizeof(REAL));
	dtcm_buffer_y = (REAL *) sark_alloc (SEGSIZE, sizeof(REAL));

	moc_count_buffer = (uint *) sark_alloc (MOC_DELAY_MS,sizeof(uint));

	if (dtcm_buffer_a == NULL ||dtcm_buffer_b == NULL ||dtcm_buffer_x == NULL ||dtcm_buffer_y == NULL 
			||  sdramout_buffer == NULL || moc_count_buffer == NULL)
	{
		test_DMA = FALSE;
		//io_printf (IO_BUF, "[core %d] error - cannot allocate buffer\n", coreID);
	}
	else
	{
		test_DMA = TRUE;
		// initialize sections of DTCM, system RAM and SDRAM
		for (uint i = 0; i < SEGSIZE; i++)
		{
			dtcm_buffer_a[i]   = 0;
			dtcm_buffer_b[i]   = 0;
		}
		for (uint i = 0; i < SEGSIZE; i++)
		{
			dtcm_buffer_x[i]   = 0;
			dtcm_buffer_y[i]   = 0;
		}
		for (uint i=0;i<data_size;i++)
		{
			sdramout_buffer[i]  = 0;
		}
		for (uint i=0;i<MOC_DELAY_MS;i++)
		{
            moc_count_buffer[i] = 0;
		}
        MC_seg_idx=0;
	}

	
	//============MODEL INITIALISATION================//
    REAL complex lin_z1,lin_z2,lin_z3,lin_tf,nlin_z1,nlin_z2,nlin_z3,nlin_tf;
    REAL rateToAttentuationFactor,lin_cf,nlBWp,nlBWq,linBWp,linBWq,linCFp,linCFq,
        nlin_bw,nlin_phi,nlin_theta,nlin_cos_theta,nlin_sin_theta,nlin_alpha,
        lin_bw,lin_phi,lin_theta,lin_cos_theta,lin_sin_theta,lin_alpha;

	//set center frequency
	//cf=4000.0;
	cf=(REAL)drnl_cf;

	//non-linear pathway
	nlBWq=180.0;//58.7147428141;//
	nlBWp=0.14;//0.0342341965;//
	nlin_bw=nlBWp * cf + nlBWq;
	nlin_phi=2.0 * M_PI * nlin_bw * dt;
	nlin_theta= 2.0 * M_PI * cf * dt;
	nlin_cos_theta= cos(nlin_theta);
	nlin_sin_theta= sin(nlin_theta);
	nlin_alpha= -exp(-nlin_phi) * nlin_cos_theta;
	nlin_a1= 2.0 * nlin_alpha;
	nlin_a2= exp(-2.0 * nlin_phi);
	nlin_z1 = (1.0 + nlin_alpha * nlin_cos_theta) - (nlin_alpha * nlin_sin_theta) * _Complex_I;
	nlin_z2 = (1.0 + nlin_a1 * nlin_cos_theta) - (nlin_a1 * nlin_sin_theta) * _Complex_I;
	nlin_z3 = (nlin_a2 * cos(2.0 * nlin_theta)) - (nlin_a2 * sin(2.0 * nlin_theta)) * _Complex_I;
	nlin_tf = (nlin_z2 + nlin_z3) / nlin_z1;
	nlin_b0 = cabs(nlin_tf);
	nlin_b1 = nlin_alpha * nlin_b0;

	//compression algorithm variables
	a=30e4;//5e4;
	c=0.25k;
	ctBM = 1e-9 * pow(10.0,32.0/20.0);
	recip_ctBM=1.0/ctBM;
	dispThresh=ctBM/a;

	//linear pathway
	lin_gain=200.0;
	linBWq=235.0;//76.65535867396389;//
	linBWp=0.2;//0.048905995;//
	lin_bw=linBWp * cf + linBWq;
	lin_phi=2.0 * M_PI * lin_bw * dt;
	linCFp=0.62;
	linCFq=266.0;
	lin_cf=linCFp*cf+linCFq;
	lin_theta= 2.0 * M_PI * lin_cf * dt;
	lin_cos_theta= cos(lin_theta);
	lin_sin_theta= sin(lin_theta);
	lin_alpha= -exp(-lin_phi) * lin_cos_theta;
	lin_a1= 2.0 * lin_alpha;
	lin_a2= exp(-2.0 * lin_phi);
	lin_z1 = (1.0 + lin_alpha * lin_cos_theta) - (lin_alpha * lin_sin_theta) * _Complex_I;
	lin_z2 = (1.0 + lin_a1 * lin_cos_theta) - (lin_a1 * lin_sin_theta) * _Complex_I;
	lin_z3 = (lin_a2 * cos(2.0 * lin_theta)) - (lin_a2 * sin(2.0 * lin_theta)) * _Complex_I;
	lin_tf = (lin_z2 + lin_z3) / lin_z1;
	lin_b0 = cabs(lin_tf);
	lin_b1 = lin_alpha * lin_b0;

	//starting values
	lin_x1=0.0;
	lin_y1[0]=0.0;
	lin_y1[1]=0.0;
	
	lin_y2[0]=0.0;
	lin_y2[1]=0.0;	

	nlin_x1a=0.0;
	nlin_y1a[0]=0.0;
	nlin_y1a[1]=0.0;

	nlin_y2a[0]=0.0;
	nlin_y2a[1]=0.0;

	nlin_x1b=0.0;
	nlin_y1b[0]=0.0;
	nlin_y1b[1]=0.0;
	
	nlin_y2b[0]=0.0;
	nlin_y2b[1]=0.0;

	rateToAttentuationFactor = 20e4;

	MOCnow1=0.0;
	MOCnow2=0.0;
	MOCnow3=0.0;

	MOCtau[0] = 0.055;
	MOCtau[1] = 0.4;
    MOCtau[2] = 1;

    MOCtauweights[0] = 0.9;
    MOCtauweights[1] = 0.1;
    MOCtauweights[2] = 0;

    MOCdec1 = exp(- dt/MOCtau[0]);
    MOCdec2 = exp(- dt/MOCtau[1]);
    MOCdec3 = exp(- dt/MOCtau[2]);

    MOCfactor1 = 0.01 * rateToAttentuationFactor * MOCtauweights[0] * dt;
    MOCfactor2 = 0.01 * rateToAttentuationFactor * MOCtauweights[1] * dt;
    MOCfactor3 = 0.01 * rateToAttentuationFactor * MOCtauweights[2] * dt;

    MOCspikeCount=0;

#ifdef PROFILE
    profiler_init(
        data_specification_get_region(1, data_address));
#endif
}

bool check_incoming_spike_id(uint spike){
    //find corresponding key_mask_index entry
    uint32_t imin = 0;
    uint32_t imax = n_mocs;

    while (imin < imax) {
        int imid = (imax + imin) >> 1;
        key_mask_table_entry entry = key_mask_table[imid];
        if ((spike & entry.mask) == entry.key){
            uint neuron_id = spike & ~entry.mask;
            last_neuron_info.e_index = entry.conn_index;
            last_neuron_info.w_index = neuron_id/32;
            last_neuron_info.id_shift = 31-(neuron_id%32);
	        return(moc_conn_lut[last_neuron_info.e_index+last_neuron_info.w_index] & (uint32_t)1 << last_neuron_info.id_shift);
        }
        else if (entry.key < spike) {

            // Entry must be in upper part of the table
            imin = imid + 1;
        } else {

            // Entry must be in lower part of the table
            imax = imid;
        }
    }
    return false;

}

void update_moc_buffer(uint sc){
    moc_count_buffer[moc_buffer_index]=sc;
    moc_buffer_index++;
    if (moc_buffer_index >= MOC_DELAY_MS) moc_buffer_index = 0;
}

uint get_current_moc_spike_count(){
    uint spike_count = 0;
    for (uint i=0;i<MOC_DELAY_MS;i++)
    {
        spike_count+=moc_count_buffer[i];
    }
return spike_count;
}

void data_write(uint null_a, uint null_b)
{
	REAL *dtcm_buffer_out;
	uint out_index;
	
	if(test_DMA == TRUE)
	{
		if(!write_switch)
		{
			out_index=index_x;
			dtcm_buffer_out=dtcm_buffer_x;
		}
		else
		{
			out_index=index_y;
			dtcm_buffer_out=dtcm_buffer_y;
		}

		spin1_dma_transfer(DMA_WRITE,&sdramout_buffer[out_index],dtcm_buffer_out,DMA_WRITE,
		  						SEGSIZE*sizeof(REAL));
	}
}

uint process_chan(REAL *out_buffer,float *in_buffer)
{
	uint segment_offset=SEGSIZE*(cbuff_index);
	uint i;		
	REAL linout1,linout2,nonlinout1a,nonlinout2a,nonlinout1b,nonlinout2b,abs_x,compressedNonlin;
	REAL filter_1;

	if(ms_counter>=n_seg_per_ms){
	    update_moc_buffer(moc_spike_count);
	    moc_spike_count = 0;
	}
	//TODO: change MOC method to a synapse model
	for(i=0;i<SEGSIZE;i++)
	{
		//Linear Path
        filter_1 = lin_b0 * in_buffer[i] + lin_b1 * lin_x1;
        linout1= filter_1 - lin_a1 * lin_y1[1] - lin_a2 * lin_y1[0];

		lin_x1=in_buffer[i];
		lin_y1[0]=lin_y1[1];
		lin_y1[1]=linout1;

        filter_1 = lin_gain*lin_b0 * linout1 + lin_b1 * lin_y1[0];
        linout2= filter_1 - lin_a1 * lin_y2[1] - lin_a2 * lin_y2[0];
		
		lin_y2[0]= lin_y2[1];
		lin_y2[1]= linout2;

		//non-linear path
		//stage 1
        filter_1 =  nlin_b0 * in_buffer[i] + nlin_b1 * nlin_x1a;
        nonlinout1a = filter_1 - nlin_a1 * nlin_y1a[1] - nlin_a2 * nlin_y1a[0];

		nlin_x1a=in_buffer[i];
		nlin_y1a[0]=nlin_y1a[1];
		nlin_y1a[1]=nonlinout1a;

        filter_1 = nlin_b0 * nonlinout1a + nlin_b1 * nlin_y1a[0];
        nonlinout2a = filter_1 - nlin_a1 * nlin_y2a[1] - nlin_a2 * nlin_y2a[0];

		nlin_y2a[0]= nlin_y2a[1];
		nlin_y2a[1]= nonlinout2a;

		//MOC efferent effects
		MOCspikeCount = (REAL)get_current_moc_spike_count();
        MOCnow1= MOCnow1* MOCdec1+ MOCspikeCount* MOCfactor1;
        MOCnow2= MOCnow2* MOCdec2+ MOCspikeCount* MOCfactor2;
        MOCnow3= MOCnow3* MOCdec3+ MOCspikeCount* MOCfactor3;
        // MOC= 1 when all MOCnow are zero
        // 0 < MOC < 1
        MOC= 1./(1+MOCnow1+MOCnow2+MOCnow3);
		nonlinout2a*=MOC;
		//stage 2
		abs_x= ABS(nonlinout2a);

		if(abs_x<dispThresh)
		{			
			compressedNonlin= a * nonlinout2a;
		}
		else
		{
			compressedNonlin=SIGN(nonlinout2a) * ctBM * (REAL)expk(c * logk((accum)(a*(abs_x*recip_ctBM))));
		}

		//stage 3
        filter_1 = nlin_b0 * compressedNonlin + nlin_b1 * nlin_x1b;
        nonlinout1b = filter_1 - nlin_a1 * nlin_y1b[1] - nlin_a2 * nlin_y1b[0];

		nlin_x1b=compressedNonlin;
		nlin_y1b[0]=nlin_y1b[1];
		nlin_y1b[1]=nonlinout1b;

        filter_1 = nlin_b0 * nonlinout1b + nlin_b1 * nlin_y1b[0];
        nonlinout2b = filter_1 - nlin_a1 * nlin_y2b[1] - nlin_a2 * nlin_y2b[0];

		nlin_y2b[0]= nlin_y2b[1];
		nlin_y2b[1]= nonlinout2b;

		//save to buffer
		out_buffer[i]=linout2 + nonlinout2b;
	}
//	MOCspikeCount = 0;
	return segment_offset;
}

void process_handler(uint null_a,uint null_b)
{
		seg_index++;
	    //check circular buffer
		if(cbuff_index<cbuff_numseg-1)
		{    //increment circular buffer index
		    cbuff_index++;
		}
		else
		{
		    cbuff_index=0;
		}
		//choose current buffers
		if(!read_switch && !write_switch)
		{
			index_x=process_chan(dtcm_buffer_x,dtcm_buffer_b);

		}
		else if(!read_switch && write_switch)
		{
			index_y=process_chan(dtcm_buffer_y,dtcm_buffer_b);
		}
		else if(read_switch && !write_switch)
		{
			index_x=process_chan(dtcm_buffer_x,dtcm_buffer_a);
		}
		else
		{
			index_y=process_chan(dtcm_buffer_y,dtcm_buffer_a);
		}
    	spin1_trigger_user_event(NULL,NULL);
}

void transfer_handler(uint tid, uint ttag)
{
	if (ttag==DMA_WRITE)
	{
	    #ifdef PROFILE
        profiler_write_entry_disable_irq_fiq(PROFILER_EXIT | PROFILER_TIMER);
        #endif
		//flip write buffers
		write_switch=!write_switch;
        //send MC packet to connected IHC/AN models
        while (!spin1_send_mc_packet(key, 0, NO_PAYLOAD))
        {
            spin1_delay_us(1);
        }
	}
	else
	{
		#ifdef PRINT
		io_printf(IO_BUF,"[core %d] invalid %d DMA tag!\n",coreID,ttag);
		#endif
	}
}

void spike_check(uint32_t rx_key,uint null){
    if (check_incoming_spike_id(rx_key)){
//        io_printf(IO_BUF,"MOC spike from %u\n",mc_key);
        moc_spike_count++;
    }
}

void moc_spike_received(uint mc_key, uint null)
{
    spin1_schedule_callback(spike_check,mc_key,NULL,1);
}

void app_end(uint null_a,uint null_b)
{
    if( sync_count<num_ihcans)
    {
        //send end MC packet to child IHCANs
        //log_info("sending final packet to IHCANs\n");
        while (!spin1_send_mc_packet(key|1, 0, NO_PAYLOAD)) {
            spin1_delay_us(1);
        }
        //wait for acknowledgment from child IHCANs
        while(sync_count<num_ihcans)
        {
           spin1_delay_us(1);
        }
    }
    //all expected acks received
    //send final ack packet back to parent OME
//    io_printf(IO_BUF,"fintxack\n");
    io_printf(IO_BUF,"spinn_exit\n");
    while (!spin1_send_mc_packet(ome_key|2, 0, NO_PAYLOAD)) {
        spin1_delay_us(1);
    }
    spin1_exit (0);
}

void data_read(uint mc_key, uint payload)
{
    if (mc_key == ome_data_key)
    {
        //payload is OME output value
        //convert payload to float
        MC_union.u = payload;
        //collect the next segment of samples and copy into DTCM
        if(test_DMA == TRUE)
        {
            MC_seg_idx++;
            #ifdef PROFILE
            if(MC_seg_idx>=SEGSIZE)profiler_write_entry_disable_irq_fiq
            (PROFILER_ENTER | PROFILER_TIMER);
            #endif
            //assign recieve buffer
            if(!read_switch)
            {
                dtcm_buffer_a[MC_seg_idx-1] = MC_union.f;
                //completed filling a segment of input values
                if(MC_seg_idx>=SEGSIZE)
                {
                    MC_seg_idx=0;
                    read_switch=1;
                    spin1_schedule_callback(process_handler,0,0,1);
                }
            }
            else
            {
                dtcm_buffer_b[MC_seg_idx-1] = MC_union.f;
                //completed filling a segment of input values
                if(MC_seg_idx>=SEGSIZE)
                {
                    MC_seg_idx=0;
                    read_switch=0;
                    spin1_schedule_callback(process_handler,0,0,1);
                }
            }
        }
    }
    else//must be a command
    {
        //extract comms command from key
        uint command = mc_key & mask;

        if(command == 1 && seg_index==0)//ready to send packet received from OME
        {
            if (sync_count<num_ihcans)//waiting for acknowledgement from child IHCANs
            {
//                io_printf(IO_BUF,"r2s\n");
                //sending ready to send MC packet to connected IHCAN models
                while (!spin1_send_mc_packet(key|1, 0, NO_PAYLOAD))
                {
                    spin1_delay_us(1);
                }
            }
        }
        else if (command == 1 && seg_index>0)//simulation finished from OME
        {
            spin1_schedule_callback(app_end,NULL,NULL,2);
        }

        else if (command == 2)//acknowledgement packet received from a child IHCAN
        {
//            io_printf(IO_BUF,"rxack\n");
            sync_count++;
            if (sync_count==num_ihcans && seg_index==0)
            {
//                io_printf(IO_BUF,"txack from %d\n",ome_key);
                //all acknowledgments have been received from the child IHCAN models
                //send acknowledgement back to parent OME
                while (!spin1_send_mc_packet(ome_key|2, 0, NO_PAYLOAD))
                {
                    spin1_delay_us(1);
                }
                sync_count=0;
            }
        }
        else while(1){};
    }
}

void app_done ()
{
    #ifdef PROFILE
	profiler_finalise();
    #endif
}

void c_main()
{
  // Get core and chip IDs
  coreID = spin1_get_core_id ();
  chipID = spin1_get_chip_id ();
  app_init();
  //setup callbacks
  //process channel once data input has been read to DTCM
  spin1_callback_on (DMA_TRANSFER_DONE,transfer_handler,0);
  spin1_callback_on (MCPL_PACKET_RECEIVED,data_read,-1);
  spin1_callback_on (MC_PACKET_RECEIVED,moc_spike_received,-1);
  spin1_callback_on (USER_EVENT,data_write,0);

  spin1_start (SYNC_WAIT);
  app_done ();
}

