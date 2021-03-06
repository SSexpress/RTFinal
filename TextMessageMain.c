#include <stm32f2xx.h>
#include <rtl.h>

#include "boardlibs\GLCD.h"
#include "boardlibs\Serial.h"
#include "boardlibs\JOY.h"
#include "boardlibs\LED.h"
#include "boardlibs\I2C.h"
#include "boardlibs\sram.h"
#include "boardlibs\KBD.h"

#include "userlibs\LinkedList.h"
#include "userlibs\dbg.h"

#include "TextMessage.h"

// set up the SRAM memory spaces for the two lists
//#define MESSAGE_STR_OFFSET 4*sizeof(ListNode)
//uint32_t *MsgRXQ = (uint32_t *)mySRAM_BASE;	// the receive queue list
//uint32_t *MsgSTR = (uint32_t *)(mySRAM_BASE+MESSAGE_STR_OFFSET);	// the long-term storage list space

ListNode *Storage = (ListNode *)mySRAM_BASE;

// the box for the receive queue
_declare_box(poolRXQ, sizeof(ListNode), 4);

// semaphores and events
OS_SEM sem_Timer10Hz;
OS_SEM sem_Timer1Hz;
OS_SEM sem_TextRx;
OS_SEM sem_dispUpdate;

// event flag masks
uint16_t timer10Hz = 0x0002;
uint16_t timer1Hz = 0x0001;

uint16_t hourButton = 0x0010;
uint16_t minButton = 0x0020;

uint16_t txtRx = 0x0001;

uint16_t dispClock = 0x0001;
uint16_t dispUser = 0x0010;
uint16_t userDelete = 0x0020;
uint16_t newMsg = 0x0040;

/*
* structures, variables, and mutexes
*/

OS_MUT mut_osTimestamp;
Timestamp osTimestamp = { 0, 0, 0 };

OS_MUT mut_msgList;
OS_MUT mut_cursor;
OS_MUT mut_LCD;
struct Cursor{
	ListNode* msg;
	uint8_t row;
};
struct Cursor cursor;
List lstRXQ = {0, NULL, NULL};
List lstStr = {0, NULL, NULL};
// List *lstStrFree;
ListNode dfltMsg;

void printToScreen(uint8_t time[], uint8_t pos,ListNode* dispNode);
void timeToString(uint8_t time[], Timestamp* timestamp);

// delcare mailbox for serial buffer
os_mbx_declare(mbx_MsgBuffer, 4);

__task void InitTask(void);
__task void TimerTask(void);
OS_TID idTimerTask;
__task void ClockTask(void);
OS_TID idClockTask;
__task void JoystickTask(void);
OS_TID idJoyTask;
__task void TextRX(void);
OS_TID idTextRX;
__task void DisplayTask(void);
OS_TID idDispTask;

void SerialInit(void);



int main(void){
	// initialize hardware
	SerialInit();
	JOY_Init();
	LED_Init();
	SRAM_Init();
	GLCD_Init();
	KBD_Init();

	// InitTask
	os_sys_init_prio(InitTask, 250);

}


/*
*
*
*		OS Tasks
*
*/

// initialization task

__task void InitTask(void){
	// initialize semaphores
	os_sem_init(&sem_Timer10Hz, 0);
	os_sem_init(&sem_Timer1Hz, 0);
	os_sem_init(&sem_TextRx, 0);
	os_sem_init(&sem_dispUpdate, 0);

	// initialize mutexes
	os_mut_init(&mut_osTimestamp);
	os_mut_init(&mut_cursor);
	os_mut_init(&mut_LCD);
	// the message input queue
	lstRXQ.count = 0;
	lstRXQ.first = NULL;
	lstRXQ.last = NULL;
	// lstRXQ->startAddr = MsgRXQ;
	// the storage lists
	os_mut_init(&mut_msgList);
	// lstStrFree->startAddr = MsgSTR;
	// used spaces
	lstStr.count = 0;
	lstStr.first = NULL;
	lstStr.last = NULL;
	// lstStrUsed->startAddr = NULL;
	
	strcpy((char*)dfltMsg.data.text, "No new messages at this time.");
	dfltMsg.data.cnt = 29;

	// initialize box for receive queue
	_init_box(poolRXQ, sizeof(poolRXQ), sizeof(ListNode));

	// This is literally the holy grail part.
	// give it the pool start (mySRAM_BASE)
	// the size of the box, which is 1000*sizeof(ListNode) plus some overhead
	// box size is the sizeof(ListNode).
	// math taken from the _declare_box() macro for overhead and alignment calcs
	_init_box(Storage, (((sizeof(ListNode)+3) / 4)*(1000) + 3) * 4, sizeof(ListNode));


	// List_init(lstRXQ);
	// List_init(lstStrFree);

	// initialize mailboxes
	os_mbx_init(&mbx_MsgBuffer, sizeof(mbx_MsgBuffer));

	// initialize tasks
	idTimerTask = os_tsk_create(TimerTask, 150);		// Timer task is relatively important.
	idJoyTask = os_tsk_create(JoystickTask, 101);
	idClockTask = os_tsk_create(ClockTask, 175);		// high prio since we need to timestamp messages
	idTextRX = os_tsk_create(TextRX, 170);
	idDispTask = os_tsk_create(DisplayTask, 100);

	GLCD_Clear(Black);
	os_evt_set(dispUser, idDispTask);
	os_evt_set(timer1Hz, idClockTask);
	//sudoku
	os_tsk_delete_self();
}

/*
*		Display Task
*
*/

__task void DisplayTask(void){
	uint8_t stime[] = "00:00:00";
	uint16_t flags;
	uint8_t delMode = FALSE;
	uint8_t select = FALSE;
	for (;;){
		os_evt_wait_or(dispUser | userDelete | newMsg, 0xffff);
		flags = os_evt_get();
		
		os_mut_wait(&mut_msgList, 0xFFFF);
		os_mut_wait(&mut_cursor, 0xffff);
		os_mut_wait(&mut_LCD, 0xffff);
		
		if((((flags & dispUser) == dispUser) || (flags & newMsg) == newMsg) && !delMode){	// if in normal mode
			if(lstStr.count != 0){
				if(lstStr.count == 1){
					cursor.msg = lstStr.last;
				}
				printToScreen(stime, cursor.row, cursor.msg);
			}else{
				printToScreen(stime, 0, &dfltMsg);
			}
		} else if((((flags & userDelete) == userDelete) && !delMode) && lstStr.count > 0){	// if entering delete mode
			delMode = TRUE;
			// display the DELETE? YES/NO messages
			GLCD_SetTextColor(White);
			GLCD_SetBackColor(Black);
			GLCD_DisplayString(8,0,1,(uint8_t*)"Delete Msg?");
			GLCD_DisplayString(9,0,1,(uint8_t*)"Yes");
			GLCD_SetTextColor(Black);
			GLCD_SetBackColor(Red);
			GLCD_DisplayString(9,4,1,(uint8_t*)"No");
			select = FALSE;
		} else if (((flags & dispUser) == dispUser) && delMode && lstStr.count > 0){ // if navigating in delete mode
			// swap between YES and NO
			select = select == FALSE ? TRUE : FALSE;
			// visually swap too
			if (select){
				GLCD_SetTextColor(Black);
				GLCD_SetBackColor(Red);
				GLCD_DisplayString(9,0,1,(uint8_t*)"Yes");
				GLCD_SetTextColor(White);
				GLCD_SetBackColor(Black);
				GLCD_DisplayString(9,4,1,(uint8_t*)"No");
			} else {
				GLCD_SetTextColor(White);
				GLCD_SetBackColor(Black);
				GLCD_DisplayString(9,0,1,(uint8_t*)"Yes");
				GLCD_SetTextColor(Black);
				GLCD_SetBackColor(Red);
				GLCD_DisplayString(9,4,1,(uint8_t*)"No");
			}
		} else if (((flags & userDelete) == userDelete) && delMode && lstStr.count > 0){ // if confirming choice.
			if (select && lstStr.count > 0){ // if we're deleting the message
				List_remove(&lstStr, cursor.msg);
				cursor.msg = cursor.msg->prev ? cursor.msg->prev : cursor.msg->next ? cursor.msg->next : NULL;
			}
			delMode = FALSE;
			select = FALSE;
			GLCD_SetBackColor(Black);
			GLCD_SetTextColor(White);
			GLCD_DisplayString(8,0,1,(uint8_t *)"           ");
			GLCD_DisplayString(9,0,1,(uint8_t *)"   ");
			GLCD_DisplayString(9,4,1,(uint8_t *)"  ");
			os_evt_set(newMsg, idDispTask);
		}
		
		// diplaying the number of total messages in storage
		GLCD_SetTextColor(White);
		GLCD_SetBackColor(Black);
		
		GLCD_DisplayChar(0,17,1,lstStr.count%10+0x30);
		GLCD_DisplayChar(0,16,1,(lstStr.count/10%10)+0x30);
		GLCD_DisplayChar(0,15,1,lstStr.count/100%10+0x30);
		GLCD_DisplayChar(0,14,1,lstStr.count/1000%10+0x30);
		
		os_mut_release(&mut_msgList);
		os_mut_release(&mut_cursor);
		os_mut_release(&mut_LCD);
		
		os_evt_clr(dispUser, idDispTask);
	}
}

void printToScreen(uint8_t time[], uint8_t pos, ListNode* dispNode){
	uint8_t i=0;
	uint8_t lineOffset=3;
	uint8_t colOffset=2;
	for(i=0;i<64;i++){
		if((i+pos*16)<dispNode->data.cnt){
			GLCD_SetTextColor(White);
			GLCD_SetBackColor(Black);
			GLCD_DisplayChar((i/16)+lineOffset, (i%16)+colOffset, 1, dispNode->data.text[i+pos*16]);
		} else {
			GLCD_DisplayChar((i/16)+lineOffset, (i%16)+colOffset, 1, *" ");
		}
	}
	timeToString(time, &(dispNode->data.time));
	GLCD_DisplayString(9,12,1,time);
}

void timeToString(uint8_t time[], Timestamp* timestamp){
	time[0] = timestamp->hours / 10 + 0x30;
	time[1] = timestamp->hours % 10 + 0x30;
	time[3] = timestamp->minutes / 10 + 0x30;
	time[4] = timestamp->minutes % 10 + 0x30;
	time[6] = timestamp->seconds / 10 + 0x30;
	time[7] = timestamp->seconds % 10 + 0x30;
}

// timer task for general-purpose polling and task triggering
__task void TimerTask(void){
	static uint32_t counter = 0;
	static uint32_t secondFreq = 1;				// frequencies in Hz of refresh rate for these events
	static uint32_t buttonInputFreq = 10;
	for (;;){
		os_dly_wait(10);	// base freq at 100Hz
		counter++;
		// set up if structure to get different timing frequencies
		if (counter % (100 / buttonInputFreq) == 0){	// 100Hz/5Hz = 20, * 10ms = 200ms = 5Hz
			os_evt_set(timer10Hz, idJoyTask);
		}
		if (counter % (100 / secondFreq) == 0){// 100*10ms = 1000ms = 1sec
			os_evt_set(timer1Hz, idClockTask);
		}
		counter %= 100; // makes sure the counter doesn't go over 100 (100*10ms = 1000ms = 1sec)
	}
}

__task void ClockTask(void){
	uint16_t flags;
	uint8_t time[] = "00:00:00";
	for (;;){
		os_evt_wait_or(timer1Hz | hourButton | minButton, 0xffff);
		flags = os_evt_get();
		if ((flags & timer1Hz) == timer1Hz){
			os_mut_wait(&mut_osTimestamp, 0xffff);

			osTimestamp.seconds++;
			osTimestamp.minutes += osTimestamp.seconds / 60;
			osTimestamp.seconds %= 60;
			osTimestamp.hours += osTimestamp.minutes / 60;
			osTimestamp.minutes %= 60;
			osTimestamp.hours %= 24;

			os_evt_clr(timer1Hz, idClockTask);
			flags = os_evt_get();
			os_mut_release(&mut_osTimestamp);
			os_evt_set(dispClock, idDispTask);
		}
		if ((flags & hourButton) == hourButton){
			os_mut_wait(&mut_osTimestamp, 0xffff);

			osTimestamp.hours++;
			osTimestamp.hours %= 24;
			os_evt_clr(hourButton, idClockTask);
			flags = os_evt_get();
			os_mut_release(&mut_osTimestamp);
			os_evt_set(dispClock, idDispTask);
		}
		if ((flags & minButton) == minButton){
			os_mut_wait(&mut_osTimestamp, 0xffff);

			osTimestamp.minutes++;
			osTimestamp.minutes %= 60;
			os_evt_clr(minButton, idClockTask);
			flags = os_evt_get();
			os_mut_release(&mut_osTimestamp);
			os_evt_set(dispClock, idDispTask);
		}
		os_mut_wait(&mut_osTimestamp, 0xffff);
		os_mut_wait(&mut_LCD,0xffff);
		GLCD_SetBackColor(Black);
		GLCD_SetTextColor(White);
		timeToString(time,&osTimestamp);
		GLCD_DisplayString(0, 0, 1, time);
		os_mut_release(&mut_osTimestamp);
		os_mut_release(&mut_LCD);
	}
}

__task void JoystickTask(void){	// TODO:  must get this working after the data handling works.  Should set events for a screen task.
	static uint32_t newJoy, oldJoy, newKeys, oldKeys = 0;
	for (;;){
		os_evt_wait_and(timer10Hz, 0xffff);
		os_mut_wait(&mut_cursor, 0xffff);
		newJoy = JOY_GetKeys();
		//os_mut_wait(&mutCursor,0xffff);
		if (newJoy != oldJoy){
			switch (newJoy){
			case JOY_DOWN:	// cursor left
				//CursorPos.col = (CursorPos.col+15)%16;	// edited to be the same as the other, for consistency.
				if(cursor.msg->prev != NULL){
					cursor.msg = cursor.msg->prev;
					cursor.row = 0;
				}
				os_evt_set(dispUser, idDispTask);
				break;
			case JOY_UP:	// cursor right
				//CursorPos.col = (CursorPos.col+1)%16;
				if(cursor.msg->next != NULL){
					cursor.msg = cursor.msg->next;
					cursor.row = 0;
			}
				os_evt_set(dispUser, idDispTask);
				break;
			case JOY_LEFT:	// cursor up
				//CursorPos.row = (CursorPos.row+5)%6;	// modulus by non-powers-of-2 is screwed up in a binary system without floats (F%6 = 3, not 6)
				os_mut_wait(&mut_msgList,0xffff);
				cursor.row = cursor.row == 0 ? 0 : (cursor.msg->data.cnt / 16) > 4 ? (cursor.row+6)%7 : 0;
				os_evt_set(dispUser, idDispTask);
				os_mut_release(&mut_msgList);
				break;
			case JOY_RIGHT:	// cursor down
				//CursorPos.row = (CursorPos.row+1)%6;
				os_mut_wait(&mut_msgList,0xffff);
				cursor.row = cursor.row == 6 ? 6 : (cursor.msg->data.cnt / 16) > 4 ? (cursor.row+1)%7 : 0;
				os_evt_set(dispUser, idDispTask);
				os_mut_release(&mut_msgList);
				break;
			case JOY_CENTER:
				os_evt_set(userDelete, idDispTask);
				break;
			default:
				break;
			}
			// send some event here
			oldJoy = newJoy;
		}
		os_mut_release(&mut_cursor);
		
		newKeys = KBD_GetKeys();
		if (newKeys != oldKeys){
			switch (newKeys){
				case 1:	// wakeup
					os_evt_set(hourButton, idClockTask);
					break;
				case 2:	// tamper
					os_evt_set(minButton, idClockTask);
					break;
			}
			oldKeys = newKeys;
		}

	}
}

// Text Parsing and Saving
__task void TextRX(void){
	static ListNode	*newmsg;
	static ListNode *message;
	for (;;){
		os_mbx_wait(&mbx_MsgBuffer, (void **)&newmsg, 0xffff);
		os_mut_wait(&mut_osTimestamp, 0xffff);
		os_mut_wait(&mut_msgList, 0xffff);

		message = _alloc_box(Storage);
		message->data = newmsg->data;
		message->data.time = osTimestamp;
		List_push(&lstStr, message);	// put our thing as the most recent message
		// TODO:  implement a "You've Got Mail" counting semaphore maybe.
		_free_box(poolRXQ, newmsg);
		os_evt_set(newMsg, idDispTask);

		os_mut_release(&mut_osTimestamp);
		os_mut_release(&mut_msgList);
	}
}


/*
*
*
*		Serial Initialization and ISR
*
*/

void SerialInit(void){
	SER_Init();
	NVIC->ISER[USART3_IRQn / 32] = (uint32_t)1 << (USART3_IRQn % 32); // enable USART3 IRQ
	NVIC->IP[USART3_IRQn] = 0xE0; // set priority for USART3 to 0xE0
	USART3->CR1 |= USART_CR1_RXNEIE; // enable device interrupt for data received and ready to read
}

void USART3_IRQHandler(void){
	static uint8_t data;	// static to keep a running tally
	static NodeData databuff;	// static buffer that just gets used over and over
	static uint8_t countData = 0;	// our place in the buffer
	static uint8_t sendflag = FALSE;
	static ListNode *rxnode;

	uint8_t flag = USART3->SR & USART_SR_RXNE; // make a flag for the USART data buffer full & ready to read signal

	if (flag == USART_SR_RXNE){	// if the flag is set.  If not, do nothing.
		data = SER_GetChar();
		if (isr_mbx_check(mbx_MsgBuffer) > 0){
			if (data >= 0x20 && data <= 0x7E){	// exclude the backspace key, include space.  TODO implement a "backspace" feature!
				databuff.text[countData] = data; // store serial input to data buffer
				countData++;
				if (countData == 160){	// if we've filled a page
					databuff.cnt = countData;	// record the size of the message
					countData = 0;	// reset the buffer data
					// send to mailbox
					sendflag = TRUE;
				}
			}else if (data == 0x7F){
				// backspace character
				countData = (countData + 159) % 160;	// move the cursor back one, and clamp at zero

			}else if (data == 0x0D){
				// return
				databuff.cnt = countData;
				// send to mailbox
				if(countData >0){
					sendflag = TRUE;
					countData = 0;
				}
				
				
			}

			if (sendflag == TRUE){
				rxnode = _alloc_box(poolRXQ);	// TODO: change to use OS stuff
				//rxnode->data.text = databuff.text;
				strcpy((char*)rxnode->data.text, (char*)databuff.text);
				rxnode->data.cnt = databuff.cnt;
				isr_mbx_send(&mbx_MsgBuffer, rxnode);
				sendflag = FALSE;
			}
		}
	}
}
