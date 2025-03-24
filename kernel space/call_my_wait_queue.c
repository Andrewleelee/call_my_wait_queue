#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <asm/current.h>
#include <linux/slab.h>
#include <linux/delay.h>

struct my_data {
    int pid;
    struct list_head list;
};

//建立一個wait queue
DECLARE_WAIT_QUEUE_HEAD(my_wait_queue);
LIST_HEAD(my_list);
static int PID_add_to_wait_queue = 0; //利用my_list中紀錄的pid，依序將process加入waitqueue

//設定條件
static int condition = 0;   //條件


void clear_my_list(void) {
    //清空my_list的內容
    struct my_data *entry, *tmp;

    list_for_each_entry_safe(entry, tmp, &my_list, list) {
        list_del(&entry->list); //刪除list中的node
        kfree(entry);          //歸還memory
    }
    INIT_LIST_HEAD(&my_list); //重設list head
}

static int enter_wait_queue(int pid){
    /*
    將當前process加入wait queue
    my_wait_queue: 建立的wait queue
    condition != 0: 條件是否滿足
    */
    int next_PID_add_to_wait_queue = -1;
    int count = 0;
    int ret;
    msleep(100); //等待其他的process進入system call並且被記錄到my_list（由於是由各個thread平行的呼叫system call，這段code也會被平行的執行）
    while(true){
        //不斷的迴圈，直到PID_add_to_wait_queue等於當前pid
        if(PID_add_to_wait_queue == 0){
            struct my_data *ptr;
            ptr = list_first_entry(&my_list, typeof(*ptr) , list);
            PID_add_to_wait_queue = ptr->pid;
            printk("Intial PID_add_to_wait_queue=%d\n", PID_add_to_wait_queue);
        }
        else if(PID_add_to_wait_queue == pid){
            struct my_data *entry;
            int curr_flag = 0;
            printk("\nEqual pid with PID_add_to_wait_queue %d\n", pid);
            //###因為此處會遍歷my_list，找到PID_add_to_wait_queue對應的下一個list node，因此需要確保list_for_each_entry被執行時，所有的thread都已經被記錄到my_list中###
            list_for_each_entry(entry, &my_list, list) {
                //找到當前PID_add_to_wait_queue對應的list node的下一個list node;
                //printk("@@@Now curr_flag = %d@@@\n", curr_flag);
                if(entry->pid == PID_add_to_wait_queue){
                    //當前list node即PID_add_to_wait_queue值對應的list node
                    printk("\nFind the list node  PID_add_to_wait_queue = pid %d\n", entry->pid);
                    curr_flag = 1;
                    printk("###Set The curr_flag, curr_flag=%d###\n", curr_flag);
                }
                else if(curr_flag == 1){
                    next_PID_add_to_wait_queue = entry->pid;
                    printk("\nnext_PID_add_to_wait_queue = %d\n", next_PID_add_to_wait_queue);
                    break;
                }
            }
            break;
        }
        else if(count == 20){
            printk("ERROR: no equal PID_add_to_wait_queue, now PID_add_to_wait_queue = %d\n", PID_add_to_wait_queue);
            break;
        }
        else{
            msleep(100);
            count++;
            //printk("process %d run the loop count=%d\n", pid, count);
            continue;
        }

    }

    printk("Added process with pid=%d to wait queue\n", pid);
    printk("PID_add_to_wait_queue = %d\n", PID_add_to_wait_queue);
    //process進入wait queue的順序應是FIFO的，但是由於某些原因，在各個process中，從他們print "enter wait queue thread_id:....."到他們呼叫system call將自己加入wait queue之間產生了時間差，導致看到的加入wait queue與wake up順序非FIFO
    //(可能是各個process宣稱自己加入了wait queue到他們真正的進入wait queue之間執行的指令導致看起來沒有FIFO)
    PID_add_to_wait_queue = next_PID_add_to_wait_queue; 
    wait_event_interruptible(my_wait_queue, condition == pid);  // @@@@@thread會在此處停止，待到wake up後繼續@@@@@
    //呼叫此sys call的process會被放入wait queue (my_wait_queue)，直到條件滿足（condition == 1）時被喚醒
    ret = wait_event_interruptible(my_wait_queue, condition != 0);
    if(ret != 0){
        //wait_event_interruptible成功時會回傳0，失敗時會回傳其他值
        //故ret不等於0代表失敗
        printk("sleep failing");
        return 0;
    }
    //呼叫此sys call的process會被放入wait queue (my_wait_queue)，直到條件滿足（condition == 1）時被喚醒
    printk("sleep success!\n");
    return 1;
}
static int clean_wait_queue(void){
    /*
    從wait queue移出node
    將process放入wait queue時用的是wait_event_interruptible
    在wait_event_interruptible中，使用的是prepare_to_wait
    ###新增的wait queue node的flags會被設為~WQ_FLAG_EXCLUSIVE (0)###
    ###當條件滿足時，wait queue中所有flags = ~WQ_FLAG_EXCLUSIVE的node都會被喚醒###
    從之後測試的程式碼中可以看到，在將所有新增的thread加入wait queue中之後
    主程式會在等待1秒後開始clean wait queue的動作(即呼叫此函式)
    故**以下指令是由主程式呼叫而執行**
    */
    struct my_data *entry;
    list_for_each_entry(entry, &my_list, list) {
        condition = entry->pid;
        printk("wake up pid=%d\n", condition);
        wake_up_interruptible(&my_wait_queue); // 將wait queue中的process喚醒
        msleep(100);
    }
    if(list_empty(&(my_wait_queue.head))) {
        //藉由檢查my_wait_queue是不是空的作為喚醒成功的依據
        printk("The wait queue is empty\n");
        return 1;
    }

    return 0;
}
SYSCALL_DEFINE1(call_my_wait_queue, int, id){
    switch (id){
        case 1:
            //將當前process的pid記錄在my_list中
            struct my_data *entry;
            int pid = (int)current->pid;    
            printk("process PID=%d enter syscall\n", pid);
            entry = kmalloc(sizeof(*entry), GFP_KERNEL);
            entry->pid = pid;
            list_add_tail(&entry->list, &my_list);  //將當前process的pid記錄在list之中後，按照list之中的順序將process加入wait queue
            //將當前process加入wait queue
            enter_wait_queue(pid);
            break;
        case 2:
            //此功能會由主程式執行
            clean_wait_queue();
            msleep(1000);

            //重設相關變數，以利下次執行
            PID_add_to_wait_queue = 0;
            condition = 0;
            clear_my_list();

            break;
    }
    return 0;
}



