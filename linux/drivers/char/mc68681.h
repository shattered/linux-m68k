#ifndef _MC68681_H_
#define _MC68681_H_

/* 
 * This describes an MC68681 DUART. It has almost only overlayed registers, which
 * the structure very ugly.
 * Note that the ri-register isn't really a register of the duart but a kludge of bsc
 * to make the ring indicator available.
 * 
 * The data came from the MFC-31-Developer Kit (from Ralph Seidel,
 * zodiac@darkness.gun.de) and the data sheet of Phillip's clone device (SCN68681)
 * (from Richard Hirst, srh@gpt.co.uk)
 *
 * 11.11.95 copyright Joerg Dorchain (dorchain@mpi-sb.mpg.de)
 *
 */

struct duarthalf {
union {
volatile u_char mr1; /* rw */
volatile u_char mr2; /* rw */
}  mr;
volatile u_char ri;   /* special, read */
union {
volatile u_char sr;  /* read */
volatile u_char csr; /* write */
} sr_csr;
u_char pad1;
volatile u_char cr; /* write */
u_char pad2;
union {
volatile u_char rhr; /* read */
volatile u_char thr; /* write */
} hr;
u_char pad3;
};

struct duart {
struct duarthalf pa;
union {
volatile u_char ipcr; /* read */
volatile u_char acr;  /* write */
} ipcr_acr;
u_char pad1;
union {
volatile u_char isr; /* read */
volatile u_char imr; /* write */
} ir;
u_char pad2;
volatile u_char ctu;
u_char pad3;
volatile u_char ctl;
u_char pad4;
struct duarthalf pb;
volatile u_char ivr;
u_char pad5;
union {
volatile u_char ipr; /* read */
volatile u_char opcr; /* write */
} ipr_opcr;
u_char pad6;
union {
volatile u_char start; /* read */
volatile u_char sopc; /* write */
} start_sopc;
u_char pad7;
union {
volatile u_char stop; /* read */
volatile u_char ropc; /* write */
} stop_ropc;
u_char pad8;
};

#define MR1_BITS 3
#define MR1_5BITS 0
#define MR1_6BITS 1
#define MR1_7BITS 2
#define MR1_8BITS 3

#define MR1_PARITY_ODD 4

#define MR1_PARITY 24
#define MR1_PARITY_WITH 0
#define MR1_PARITY_FORCE 8
#define MR1_PARITY_NO 16
#define MR1_PARITY_MULTIDROP 24

#define MR1_ERROR_BLOCK 32
#define MR1_FFULL_IRQ 64
#define MR1_RxRTS_ON 128

#define MR2_STOPS 15
#define MR2_1STOP 7
#define MR2_2STOP 15

#define MR2_CTS_ON 16
#define MR2_TxRTS_ON 32

#define MR2_MODE 192
#define MR2_NORMAL 0
#define MR2_ECHO 64
#define MR2_LOCALLOOP 128
#define MR2_REMOTELOOP 192

#define CR_RXCOMMAND 3
#define CR_NONE 0
#define CR_RX_ON 1
#define CR_RX_OFF 2
#define CR_TXCOMMAND 12
#define CR_TX_ON 4
#define CR_TX_OFF 8
#define CR_MISC 112
#define CR_RESET_MR 16
#define CR_RESET_RX 32
#define CR_RESET_TX 48
#define CR_RESET_ERR 64
#define CR_RESET_BREAK 80
#define CR_START_BREAK 96
#define CR_STOP_BREAK 112

#define SR_RXRDY 1
#define SR_FFULL 2
#define SR_TXRDY 4
#define SR_TXEMPT 8
#define SR_OVERRUN 16
#define SR_PARITY 32
#define SR_FRAMING 64
#define SR_BREAK 128


#endif
