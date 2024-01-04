#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "pico/audio_i2s.h"
#include "pico_pic.h"

/*
Title  : SoundBlaster DSP Emulation 
Date   : 2023-12-30
Author : Kevin Moonlight <me@yyzkevin.com>

*/

#include "isa_dma.h"

extern "C" void OPL_Pico_Mix_callback(audio_buffer_t *);


irq_handler_t SBDSP_DMA_isr_pt;
dma_inst_t dma_config;

#define DSP_VERSION_MAJOR 2
#define DSP_VERSION_MINOR 1

// Sound Blaster DSP I/O port offsets
#define DSP_RESET           0x6
#define DSP_READ            0xA
#define DSP_WRITE           0xC
#define DSP_WRITE_STATUS    0xC
#define DSP_READ_STATUS     0xE

#define OUTPUT_SAMPLERATE   49716ul     

// Sound Blaster DSP commands.
#define DSP_DMA_HS_SINGLE       0x91
#define DSP_DMA_HS_AUTO         0x90
#define DSP_DMA_ADPCM           0x7F    //creative ADPCM 8bit to 3bit
#define DSP_DMA_SINGLE          0x14    //follosed by length
#define DSP_DMA_AUTO            0X1C    //length based on 48h
#define DSP_DMA_BLOCK_SIZE      0x48    //block size for highspeed/dma
//#define DSP_DMA_DAC 0x14
#define DSP_DIRECT_ADC          0x20
#define DSP_MIDI_READ_POLL      0x30
#define DSP_MIDI_WRITE_POLL     0x38
#define DSP_SET_TIME_CONSTANT   0x40
#define DSP_DMA_PAUSE           0xD0
#define DSP_DMA_PAUSE_DURATION  0x80    //Used by Tryrian
#define DSP_ENABLE_SPEAKER      0xD1
#define DSP_DISABLE_SPEAKER     0xD3
#define DSP_DMA_RESUME          0xD4
#define DSP_IDENT               0xE0
#define DSP_VERSION             0xE1
#define DSP_WRITETEST           0xE4
#define DSP_READTEST            0xE8
#define DSP_SINE                0xF0
#define DSP_IRQ                 0xF2
#define DSP_CHECKSUM            0xF4

#define DSP_DMA_FIFO_SIZE       1024

typedef struct sbdsp_t {
    uint8_t inbox;
    uint8_t outbox;
    uint8_t test_register;
    uint8_t current_command;
    uint8_t current_command_index;

    uint16_t dma_interval;     
    int16_t dma_interval_trim;

    uint8_t  dma_buffer[DSP_DMA_FIFO_SIZE];
    volatile uint16_t dma_buffer_tail;
    volatile uint16_t dma_buffer_head;

    uint16_t dma_block_size;
    uint32_t dma_sample_count;
    uint32_t dma_sample_count_rx;

    uint8_t time_constant;
    uint16_t sample_rate;
    uint32_t sample_step;
    uint64_t cycle_us;

    uint64_t sample_offset;  
    uint8_t sample_factor;
                
    bool autoinit;    
    bool dma_enabled;
        
    volatile bool dav_pc;
    volatile bool dav_dsp;
    volatile bool dsp_busy;

    uint8_t reset_state;  
    
} sbdsp_t;

sbdsp_t sbdsp;

audio_buffer_t *opl_buffer;

void sbdsp_process(void);

uint32_t DSP_DMA_Event(Bitu val);

uint16_t sbdsp_fifo_level() {
    if(sbdsp.dma_buffer_tail < sbdsp.dma_buffer_head) return DSP_DMA_FIFO_SIZE - (sbdsp.dma_buffer_head - sbdsp.dma_buffer_tail);
    return sbdsp.dma_buffer_tail - sbdsp.dma_buffer_head;
}
void sbdsp_fifo_rx(uint8_t byte) {    
    if(sbdsp_fifo_level()+1 == DSP_DMA_FIFO_SIZE) printf("OVERRRUN");
    sbdsp.dma_buffer[sbdsp.dma_buffer_tail]=byte;        
    sbdsp.dma_buffer_tail++;
    if(sbdsp.dma_buffer_tail == DSP_DMA_FIFO_SIZE) sbdsp.dma_buffer_tail=0;
}
void sbdsp_fifo_clear() {    
    sbdsp.dma_buffer_head=sbdsp.dma_buffer_tail;
}
bool sbdsp_fifo_half() {
    if(sbdsp_fifo_level() >= (DSP_DMA_FIFO_SIZE/2)) return true;
    return false;
}

uint16_t sbdsp_fifo_tx(char *buffer,uint16_t len) {
    uint16_t level = sbdsp_fifo_level();
    if(!level) return 0;
    if(!len) return 0;
    if(len > level) len=level;
    if(sbdsp.dma_buffer_head < sbdsp.dma_buffer_tail || ((sbdsp.dma_buffer_head+len) < DSP_DMA_FIFO_SIZE)) {          
            memcpy(buffer,sbdsp.dma_buffer+sbdsp.dma_buffer_head,len);
            sbdsp.dma_buffer_head += len;
            return len;
    }           
    else {                
        memcpy(buffer,sbdsp.dma_buffer+sbdsp.dma_buffer_head,DSP_DMA_FIFO_SIZE-sbdsp.dma_buffer_head);
        memcpy(buffer+256-sbdsp.dma_buffer_head,sbdsp.dma_buffer,len-(DSP_DMA_FIFO_SIZE-sbdsp.dma_buffer_head));        
        sbdsp.dma_buffer_head += (len-DSP_DMA_FIFO_SIZE);
        return len;
    }
    return 0;    
}

void sbdsp_dma_disable() {
    sbdsp.dma_enabled=false;    
    PIC_RemoveEvents(DSP_DMA_Event);  
}

void sbdsp_dma_enable() {    
    if(!sbdsp.dma_enabled) {
        sbdsp_fifo_clear();
        sbdsp.dma_enabled=true;            
        PIC_AddEvent(DSP_DMA_Event,sbdsp.dma_interval,1);
    }
    else {
        //printf("INFO: DMA Already Enabled");        
    }
}

uint32_t DSP_DMA_Event(Bitu val) {   
    if(sbdsp_fifo_half()) {
        sbdsp.dma_interval_trim = 5;        
    }
    else {
        sbdsp.dma_interval_trim=-5;
    }        
    
    DMA_Start_Write(&dma_config);    
    return 0;
}

void sbdsp_dma_isr(void) {
    const uint32_t dma_data = DMA_Complete_Write(&dma_config);    
    sbdsp_fifo_rx(dma_data & 0xFF);    
    sbdsp.dma_sample_count_rx++;    
    if(sbdsp.dma_sample_count_rx <= sbdsp.dma_sample_count) {        
        PIC_AddEvent(DSP_DMA_Event,sbdsp.dma_interval+sbdsp.dma_interval_trim,1);
    }
    else {                  
        if(sbdsp.autoinit) {            
            sbdsp.dma_sample_count_rx=0;            
            PIC_AddEvent(DSP_DMA_Event,sbdsp.dma_interval+sbdsp.dma_interval_trim,1);         
        }
        else {            
            sbdsp_dma_disable();            
        }
        PIC_ActivateIRQ();                                     
    }    
}


void sbdsp_mix(audio_buffer_t *buffer) {
    uint16_t i,x,y,req_bytes,rx_bytes;
    int16_t *samples, *opl_samples;       
    uint32_t offset;          
    uint32_t step;

    uint64_t starting_offset;

    char buf[1024];
    buffer->sample_count=0;
    samples = (int16_t *) buffer->buffer->bytes;         
    opl_samples = (int16_t *) opl_buffer->buffer->bytes;         
    if(sbdsp.dma_enabled) {        
        while(!sbdsp_fifo_level()) {//TODO: This is not ideal to block here.                        
        }                
        if(1==1) {//sbdsp_fifo_level() >= 16 ) {//TODO: this will be problem at the end.
            x = sbdsp_fifo_tx(buf,4);            
            starting_offset = sbdsp.sample_offset >> 16;   
            for(i=0;i<(x*sbdsp.sample_factor);i++) {        
                y=(sbdsp.sample_offset >> 16) - starting_offset;
                if(y >= x) break;        
                samples[(i<<1)+0] = buf[y]-0x80 << 8;              
                samples[(i<<1)+1] = buf[y]-0x80 << 8;                      
                sbdsp.sample_offset += sbdsp.sample_step;
            }        
            buffer->sample_count = i;
            buffer->max_sample_count=i;        
        }
        else {
            buffer->sample_count=0;                        
        }
    }        

    if(!buffer->sample_count) {      
        buffer->sample_count=1;             
        for(x=0;x<buffer->sample_count*2;x++) samples[x]=0;         
    }
    
    opl_buffer->max_sample_count = buffer->sample_count;  
    OPL_Pico_Mix_callback(opl_buffer);                        
    for(x=0;x<buffer->sample_count*2;x++) {        
        samples[x] += opl_samples[x];
    } 
       
    
    return;    
}

void sbdsp_init() {    
    uint8_t x,y,z;    
    char buffer[32];
       

    puts("Initing ISA DMA PIO...");    
    SBDSP_DMA_isr_pt = sbdsp_dma_isr;
    dma_config = DMA_init(pio0, SBDSP_DMA_isr_pt);         

    opl_buffer = (audio_buffer_t *) malloc(sizeof(audio_buffer_t));
    opl_buffer->buffer = (mem_buffer_t *) malloc(sizeof(mem_buffer_t));        
    opl_buffer->buffer->size = 512*4; // 49716/8000*64==397
    opl_buffer->buffer->bytes = (uint8_t *) malloc(opl_buffer->buffer->size);



}


void sbdsp_output(uint8_t value) {
    sbdsp.outbox=value;
    sbdsp.dav_pc=1;    
}


void sbdsp_process(void) {    
    if(sbdsp.reset_state) return;     
    sbdsp.dsp_busy=1;

    if(sbdsp.dav_dsp) {
        if(!sbdsp.current_command) {            
            sbdsp.current_command = sbdsp.inbox;
            sbdsp.current_command_index=0;
            sbdsp.dav_dsp=0;
        }
    }

    switch(sbdsp.current_command) {  
        case DSP_DMA_PAUSE:
            sbdsp.current_command=0;                                    
            sbdsp_dma_disable();
            //printf("(0xD0)DMA PAUSE\n\r");            
            break;
        case DSP_DMA_RESUME:
            sbdsp.current_command=0;
            sbdsp_dma_enable();                        
            //printf("(0xD4)DMA RESUME\n\r");                                            
            break;
        case DSP_DMA_AUTO:     
            //printf("(0x1C)DMA_AUTO\n\r");                   
            sbdsp.autoinit=1;           
            sbdsp.dma_sample_count = sbdsp.dma_block_size;
            sbdsp_dma_enable();            
            sbdsp.current_command=0;                 
            break;        
        case DSP_DMA_HS_AUTO:
            //printf("(0x90) DMA_HS_AUTO\n\r");            
            sbdsp.dav_dsp=0;
            sbdsp.current_command=0;  
            sbdsp.autoinit=1;
            sbdsp.dma_sample_count = sbdsp.dma_block_size;
            sbdsp_dma_enable();            
            break;            

        case DSP_SET_TIME_CONSTANT:
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) {
                    //printf("(0x40) DSP_SET_TIME_CONSTANT\n\r");                                
                    sbdsp.time_constant = sbdsp.inbox;
                    sbdsp.sample_rate = 1000000ul / (256 - sbdsp.time_constant);           
                    sbdsp.dma_interval = 1000000ul / sbdsp.sample_rate; // redundant.                    
                    sbdsp.sample_step = sbdsp.sample_rate * 65535ul / OUTPUT_SAMPLERATE;                    
                    sbdsp.sample_factor = (OUTPUT_SAMPLERATE / sbdsp.sample_rate)+5; //Estimate

                    //sbdsp.i2s_buffer_size = ((OUTPUT_SAMPLERATE * 65535ul) / sbdsp.sample_rate * sbdsp.dma_buffer_size) >> 16;
                    
                    
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;                    
                }    
                sbdsp.current_command_index++;
            }
            break;
        case DSP_DMA_BLOCK_SIZE:            
            if(sbdsp.dav_dsp) {                             
                if(sbdsp.current_command_index==1) {                    
                    sbdsp.dma_block_size=sbdsp.inbox;
                    sbdsp.dav_dsp=0;                    
                }
                else if(sbdsp.current_command_index==2) {                    
                    sbdsp.dma_block_size += (sbdsp.inbox << 8);
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;          
                    //printf("(0x48) Set BlockSize:%u\n\r",sbdsp.dma_block_size);                                        
                }
                sbdsp.current_command_index++;
            }
            break;          
        
        case DSP_DMA_HS_SINGLE:
            //printf("(0x91) DMA_HS_SINGLE\n\r");            
            sbdsp.dav_dsp=0;
            sbdsp.current_command=0;  
            sbdsp.autoinit=0;
            sbdsp.dma_sample_count = sbdsp.dma_block_size;
            sbdsp_dma_enable();            
            break;            
        case DSP_DMA_SINGLE:              
            if(sbdsp.dav_dsp) {            
                if(sbdsp.current_command_index==1) {
                    sbdsp.dma_sample_count = sbdsp.inbox;
                    sbdsp.dav_dsp=0;                    
                }
                else if(sbdsp.current_command_index==2) {
                    //printf("(0x14)DMA_SINGLE\n\r");                      
                    sbdsp.dma_sample_count += (sbdsp.inbox << 8);
                    sbdsp.dma_sample_count_rx=0;                    
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;  
                    sbdsp.autoinit=0;                                  
                    //printf("Sample Count:%u\n",sbdsp.dma_sample_count);                                        
                    sbdsp_dma_enable();                                                                                                                            
                }
                sbdsp.current_command_index++;
            }                        
            break;            
        case DSP_IRQ:
            sbdsp.current_command=0;             
            PIC_ActivateIRQ();                
            break;            
        case DSP_VERSION:
            if(sbdsp.current_command_index==0) {
                sbdsp.current_command_index=1;
                sbdsp_output(DSP_VERSION_MAJOR);
            }
            else {
                if(!sbdsp.dav_pc) {
                    sbdsp.current_command=0;                    
                    sbdsp_output(DSP_VERSION_MINOR);
                }
                
            }
            break;
        case DSP_IDENT:
            if(sbdsp.dav_dsp) {            
                if(sbdsp.current_command_index==1) {                    
                    //printf("(0xE0) DSP_IDENT\n\r");
                    sbdsp.dav_dsp=0;                    
                    sbdsp.current_command=0;        
                    sbdsp_output(~sbdsp.inbox);                                        
                }                
                sbdsp.current_command_index++;
            }                                       
            break;
        case DSP_ENABLE_SPEAKER:
            //printf("ENABLE SPEAKER\n");
            sbdsp.current_command=0;
            break;        
        case DSP_DISABLE_SPEAKER:
            //printf("DISABLE SPEAKER\n");
            sbdsp.current_command=0;
            break;        
        //case DSP_DIRECT_ADC:
        //case DSP_MIDI_READ_POLL:
        //case DSP_MIDI_WRITE_POLL:
        
        //case DSP_HALT_DMA:       
        case DSP_WRITETEST:
            if(sbdsp.dav_dsp) {            
                if(sbdsp.current_command_index==1) {                    
                    //printf("(0xE4) DSP_WRITETEST\n\r");
                    sbdsp.test_register = sbdsp.inbox;
                    sbdsp.dav_dsp=0;                    
                    sbdsp.current_command=0;                                                
                }                
                sbdsp.current_command_index++;
            }                                       
            break;
        case DSP_READTEST:
            if(sbdsp.current_command_index==0) {
                sbdsp.current_command=0;
                sbdsp_output(sbdsp.test_register);
            }
            break;
        
        //case DSP_SINE:
        //case DSP_CHECKSUM:          
        case 0:
            //not in a command
            break;            
        default:
            //printf("Unknown Command: %x\n",sbdsp.current_command);
            sbdsp.current_command=0;
            break;

    }                
    sbdsp.dsp_busy=0;
}
void sbdsp_reset(uint8_t value) {
    //TODO: COLDBOOT ? WARMBOOT ?    
    switch(value) {
        case 1:                        
            sbdsp.autoinit=0;
            sbdsp.dma_enabled=false;
            sbdsp.reset_state=1;
            break;
        case 0:
            if(sbdsp.reset_state==0) return; 
            if(sbdsp.reset_state==1) {                
                sbdsp.reset_state=0;                
                sbdsp.outbox = 0xAA;
                sbdsp.dav_pc=1;
                sbdsp.current_command=0;
                sbdsp.current_command_index=0;

                sbdsp.dma_block_size=0x7FF; //default per 2.01
                sbdsp.dma_sample_count=0;
                sbdsp.dma_sample_count_rx=0;              

            }
            break;
        default:
            break;
    }
}

uint8_t sbdsp_read(uint8_t address) {    
    uint8_t x;            
    switch(address) {        
        case DSP_READ:
            sbdsp.dav_pc=0;
            return sbdsp.outbox;
        case DSP_READ_STATUS: //e
            PIC_DeActivateIRQ();
            //printf("i");
            return (sbdsp.dav_pc << 7);            
        case DSP_WRITE_STATUS://c                        
            return (sbdsp.dav_dsp | sbdsp.dsp_busy) << 7;                                
        default:
            //printf("SB READ: %x\n\r",address);
            return 0xFF;            
    }

}
void sbdsp_write(uint8_t address, uint8_t value) {    
    switch(address) {         
        case DSP_WRITE://c
            if(sbdsp.dav_dsp) printf("WARN - DAV_DSP OVERWRITE\n");
            sbdsp.inbox = value;
            sbdsp.dav_dsp = 1;            
            break;            
        case DSP_RESET:
            sbdsp_reset(value);
            break;        
        default:
            //printf("SB WRITE: %x => %x \n\r",value,address);            
            break;
    }
}