#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <linux/ioctl.h>

#include<pthread.h>
#include <sys/time.h>


// Define magic for RPU Unit
#define RPU_IOC_MAGIC '$'
// for params set which using pointer
#define RPU_IOC_SET   _IOW(RPU_IOC_MAGIC, 0, int)
// for params get which using pointer
#define RPU_IOC_GET   _IOW(RPU_IOC_MAGIC, 1, int)
// The Max cmd number for ioctl, you should modify it if you add new cmd
#define RPU_IOC_MAXNR 1


/* System call definitions */
#define OPEN_SYSCALL_ID		1
#define CLOSE_SYSCALL_ID		2
#define WRITE_SYSCALL_ID		3
#define READ_SYSCALL_ID		4
#define ACK_STATUS_ID			5
#define TERM_SYSCALL_ID		6

#define RPC_BUFF_SIZE 512
#define LOGGER_CHANNEL_READY_TO_CLOSE "logger_channel_ready_to_close"

#define FILE_NAME_LEN		50


// for rpu cmd
#define RPU_SET_PARAMS		0x8001		// for set
#define RPU_GET_PARAMS	0x0001		// for get
struct rpu_cmd{
	int cmd_id;
	char data[256];
};


struct _rpc_data {
	struct rpmsg_channel *rpmsg_chnl;
	struct rpmsg_endpoint *rp_ept;
	void *rpc_lock;
	void *sync_lock;
	void *data;
};

struct _sys_call_args {
	int32_t int_field1;
	int32_t int_field2;
	uint32_t data_len;
	char data[0];
};

/* System call rpc data structure */
struct _sys_rpc {
	uint32_t id;
	struct _sys_call_args	sys_call_args;
};


struct _rpmsg_data {
	int active;
	int rpmsg_dev_fd;
	struct _sys_rpc *rpc;
	struct _sys_rpc *rpc_response;
	char *firmware_path;
};

static struct _rpmsg_data *rpmsg_device;
char sbuf[512];
int r5_id = 1;
static pthread_t ntid;
static int thread_exit = 0; 


int handle_open(struct _sys_rpc *rpc)
{
	int fd;
	ssize_t bytes_written;

	/* Open remote fd */
	fd = open(rpc->sys_call_args.data, rpc->sys_call_args.int_field1,
			rpc->sys_call_args.int_field2);

	/* Construct rpc response */
	rpmsg_device->rpc_response->id = OPEN_SYSCALL_ID;
	rpmsg_device->rpc_response->sys_call_args.int_field1 = fd;
	rpmsg_device->rpc_response->sys_call_args.int_field2 = 0; /*not used*/
	rpmsg_device->rpc_response->sys_call_args.data_len = 0; /*not used*/

	/* Transmit rpc response */
	bytes_written = write(rpmsg_device->rpmsg_dev_fd, rpmsg_device->rpc_response,
				sizeof(struct _sys_rpc));

	return (bytes_written != sizeof(struct _sys_rpc)) ? -1 : 0;
}

int handle_close(struct _sys_rpc *rpc)
{
	int retval;
	ssize_t bytes_written;

	/* Close remote fd */
	retval = close(rpc->sys_call_args.int_field1);

	/* Construct rpc response */
	rpmsg_device->rpc_response->id = CLOSE_SYSCALL_ID;
	rpmsg_device->rpc_response->sys_call_args.int_field1 = retval;
	rpmsg_device->rpc_response->sys_call_args.int_field2 = 0; /*not used*/
	rpmsg_device->rpc_response->sys_call_args.data_len = 0; /*not used*/

	/* Transmit rpc response */
	bytes_written = write(rpmsg_device->rpmsg_dev_fd, rpmsg_device->rpc_response,
				sizeof(struct _sys_rpc));

	return (bytes_written != sizeof(struct _sys_rpc)) ? -1 : 0;
}

int handle_read(struct _sys_rpc *rpc)
{
	ssize_t bytes_read, bytes_written;
	size_t  payload_size;
	char *buff = rpmsg_device->rpc_response->sys_call_args.data;

	if (rpc->sys_call_args.int_field1 == 0)
		/* Perform read from fd for large size since this is a
		STD/I request */
		bytes_read = read(rpc->sys_call_args.int_field1, buff, 512);
	else
		/* Perform read from fd */
		bytes_read = read(rpc->sys_call_args.int_field1, buff, rpc->sys_call_args.int_field2);

	/* Construct rpc response */
	rpmsg_device->rpc_response->id = READ_SYSCALL_ID;
	rpmsg_device->rpc_response->sys_call_args.int_field1 = bytes_read;
	rpmsg_device->rpc_response->sys_call_args.int_field2 = 0; /* not used */
	rpmsg_device->rpc_response->sys_call_args.data_len = bytes_read;

	payload_size = sizeof(struct _sys_rpc) +
			((bytes_read > 0) ? bytes_read : 0);

	/* Transmit rpc response */
	bytes_written = write(rpmsg_device->rpmsg_dev_fd, rpmsg_device->rpc_response, payload_size);

	return (bytes_written != payload_size) ? -1 : 0;
}

int handle_write(struct _sys_rpc *rpc)
{
	ssize_t bytes_written;

	/* Write to remote fd */
	bytes_written = write(rpc->sys_call_args.int_field1, rpc->sys_call_args.data, rpc->sys_call_args.int_field2);

	/* Construct rpc response */
	rpmsg_device->rpc_response->id = WRITE_SYSCALL_ID;
	rpmsg_device->rpc_response->sys_call_args.int_field1 = bytes_written;
	rpmsg_device->rpc_response->sys_call_args.int_field2 = 0; /*not used*/
	rpmsg_device->rpc_response->sys_call_args.data_len = 0; /*not used*/

	/* Transmit rpc response */
	bytes_written = write(rpmsg_device->rpmsg_dev_fd, rpmsg_device->rpc_response, sizeof(struct _sys_rpc));

	return (bytes_written != sizeof(struct _sys_rpc)) ? -1 : 0;
}

int handle_rpc(struct _sys_rpc *rpc)
{
	int retval;
	char *data = (char *)rpc;
	if (!strcmp(data, LOGGER_CHANNEL_READY_TO_CLOSE)) {
		rpmsg_device->active = 0;
		return 0;
	}

	/* Handle RPC */
	switch ((int)(rpc->id)) {
		case OPEN_SYSCALL_ID:
		{
			retval = handle_open(rpc);
			break;
		}
		case CLOSE_SYSCALL_ID:
		{
			retval = handle_close(rpc);
			break;
		}
		case READ_SYSCALL_ID:
		{
			retval = handle_read(rpc);
			break;
		}
		case WRITE_SYSCALL_ID:
		{
			retval = handle_write(rpc);
			break;
		}
		default:
		{
			printf("\r\nMaster>Err:Invalid RPC sys call ID: %d:%d! \r\n", rpc->id,WRITE_SYSCALL_ID);
			retval = -1;
			break;
		}
	}

	return retval;
}

int terminate_rpc_app()
{
	int bytes_written;
	int msg = TERM_SYSCALL_ID;
	printf ("Master> sending shutdown signal.\n");
	bytes_written = write(rpmsg_device->rpmsg_dev_fd, &msg, sizeof(int));
	return bytes_written;
}

/* write a string to an existing and writtable file */
int file_write(char *path, char *str)
{
	int fd;
	ssize_t bytes_written;
	size_t str_sz;

	fd = open(path, O_WRONLY);
	if (fd == -1) {
		perror("Error");
		return -1;
	}
	str_sz = strlen(str);
	bytes_written = write(fd, str, str_sz);
	if (bytes_written != str_sz) {
	        if (bytes_written == -1) {
			perror("Error");
		} 
		close(fd);
		return -1;
	}

	if (-1 == close(fd)) {
		perror("Error");
		return -1;
	}
	return 0;
}

/* Stop remote CPU and Unload drivers */
void stop_remote(void)
{
	//system("modprobe -r rpmsg_proxy_dev_driver");
	sprintf(sbuf, "/sys/class/remoteproc/remoteproc%u/state", r5_id);
	(void)file_write(sbuf, "stop");
}

void exit_action_handler(int signum)
{
	rpmsg_device->active = 0;
}

void kill_action_handler(int signum)
{
	printf("\r\nMaster>RPC service killed !!\r\n");

	/* Send shutdown signal to remote application */
	terminate_rpc_app();

	/* wait for a while to let the remote finish cleanup */
	sleep(1);
	/* Close rpmsg_device rpmsg device */
	close(rpmsg_device->rpmsg_dev_fd);

	/* Free up resources */
	free(rpmsg_device->rpc);
	free(rpmsg_device->rpc_response);
	free(rpmsg_device);

	/* Stop remote cpu and unload drivers */
	stop_remote();
}

void display_help_msg(void)
{
	printf("\r\nLinux rpmsg_device application.\r\n");
	printf("-v	 Displays rpmsg_device application version.\n");
	printf("-f	 Accepts path of firmware to load on remote core.\n");
	printf("-r       Which core 0|1\n");
	printf("-h	 Displays this help message.\n");
}



void *logger_thread(void *arg)
{
	unsigned int bytes_rcvd;
	while (rpmsg_device->active) {
		/* Block on read for rpc requests from remote context */
		do {
			bytes_rcvd = read(rpmsg_device->rpmsg_dev_fd, rpmsg_device->rpc,RPC_BUFF_SIZE);
			if (!rpmsg_device->active)
				break;
		} while(bytes_rcvd <= 0);

		/* User event, break! */
		if (!rpmsg_device->active)
			break;

		/* Handle rpc */
		if (handle_rpc(rpmsg_device->rpc)) {
			break;
		}
	}
	thread_exit = 1;
	return ((void *)0);
}


int main(int argc, char *argv[])
{
	struct sigaction exit_action;
	struct sigaction kill_action;
	unsigned int bytes_rcvd;
	int i = 0;
	int opt = 0;
	int ret = 0;
	char *user_fw_path = 0;
	int temp ;
	static int cnt = 0;
	struct rpu_cmd set_cmd, get_cmd;
	struct  timeval start;
	struct  timeval end;
	double usedTime;
	double speed;

	/* Initialize signalling infrastructure */
	memset(&exit_action, 0, sizeof(struct sigaction));
	memset(&kill_action, 0, sizeof(struct sigaction));
	exit_action.sa_handler = exit_action_handler;
	kill_action.sa_handler = kill_action_handler;
	sigaction(SIGTERM, &exit_action, NULL);
	sigaction(SIGINT, &exit_action, NULL);
	sigaction(SIGKILL, &kill_action, NULL);
	sigaction(SIGHUP, &kill_action, NULL);


	while ((opt = getopt(argc, argv, "vhf:r:")) != -1) {
		switch (opt) {
		case 'f':
			user_fw_path = optarg;
			break;
		case 'r':
			r5_id = atoi(optarg);
			if (r5_id != 0 && r5_id != 1) {
				display_help_msg();
				return -1;
			}
			break;
		case 'v':
			printf("\r\nLinux rpmsg_device application version 1.1\r\n");
			return 0;
		case 'h':
			display_help_msg();
			return 0;
		default:
			printf("getopt return unsupported option: -%c\n",opt);
			break;
		}
	}


	/* Allocate memory for rpmsg_device data structure */
	rpmsg_device = malloc(sizeof(struct _rpmsg_data));
	if (rpmsg_device == 0) {
		printf("\r\nMaster>Failed to allocate memory.\r\n");
		return -1;
	}
	rpmsg_device->active = 1;

	/* Open rpmsg_device rpmsg device */
	i = 0;
	do {
		if(r5_id){
			rpmsg_device->rpmsg_dev_fd = open("/dev/rpmsg_rpu1", O_RDWR);
		}else{
			rpmsg_device->rpmsg_dev_fd = open("/dev/rpmsg_rpu0", O_RDWR);
		}
		sleep(1);
	} while (rpmsg_device->rpmsg_dev_fd < 0 && (i++ < 2));

	if (rpmsg_device->rpmsg_dev_fd < 0) {
		printf("\r\nMaster>Failed to open rpmsg rpmsg_device driver device file.\r\n");
		ret = -1;
		goto error0;
	}

	
	/* Allocate memory for rpc payloads */
	rpmsg_device->rpc = malloc(RPC_BUFF_SIZE);
	rpmsg_device->rpc_response = malloc(RPC_BUFF_SIZE);


	/* RPC service starts */
	printf("\r\nMaster>RPC service started !!\r\n");
	if((temp=pthread_create(&ntid,NULL,logger_thread,NULL))!= 0)    {        
		printf("can't create thread: %s\n",strerror(temp));
		return 1;     
	}


	// test write
	printf("\r\nMaster>RPU1 writting test ...\r\n");
	gettimeofday(&start,NULL);
	while(1){
		set_cmd.cmd_id = RPU_SET_PARAMS;
		sprintf(set_cmd.data, "%s%d%s", "hello RPU1, i am send cmd [", cnt, "]");
		ioctl(rpmsg_device->rpmsg_dev_fd, RPU_IOC_SET, &set_cmd);
		cnt++;
		if(cnt == 1000){
			gettimeofday(&end,NULL);
			usedTime = (double)(end.tv_sec-start.tv_sec)+ ((double)(end.tv_usec-start.tv_usec))/1000000;
			speed = (double)(sizeof(get_cmd) * cnt) / (usedTime) /(1024.0);
			printf("\r\nMaster>RPU1 writting test  done !!, size = %d, usedTime : %.4f s\t(%.2f MB/s)\r\n", sizeof(get_cmd) * cnt, usedTime, speed);
			cnt = 0;
			break;
		}
	}



	// test read
	printf("\r\nMaster<RPU1 reading test ...\r\n");
	gettimeofday(&start,NULL);
	while(1){
		get_cmd.cmd_id = RPU_GET_PARAMS;
		ioctl(rpmsg_device->rpmsg_dev_fd, RPU_IOC_GET, &get_cmd);
		//printf("%s",  get_cmd.data);
		memset(&get_cmd, 0, sizeof(get_cmd));
		cnt++;
		if(cnt == 1000){
			gettimeofday(&end,NULL);
			usedTime = (double)(end.tv_sec-start.tv_sec)+ ((double)(end.tv_usec-start.tv_usec))/1000000;
			speed = (double)(sizeof(get_cmd) * cnt) / (usedTime) /(1024.0);
			printf("\r\nMaster<RPU1 reading test  done !!, size = %d, usedTime : %.4f s\t(%.2f MB/s)\r\n",sizeof(get_cmd) * cnt, usedTime, speed);
			cnt = 0;
			break;
		}
	}
	
	#if 1
	printf("\r\nMaster<-->RPU1 write and read test ...\r\n");
	while(1){
		
		set_cmd.cmd_id = RPU_SET_PARAMS;
		sprintf(set_cmd.data, "%s%d%s", "RPU1 write / read test ... [", cnt, "]");
		ioctl(rpmsg_device->rpmsg_dev_fd, RPU_IOC_SET, &set_cmd);

		get_cmd.cmd_id = RPU_GET_PARAMS;
		ioctl(rpmsg_device->rpmsg_dev_fd, RPU_IOC_GET, &get_cmd);
		//printf("%s",  get_cmd.data);
		memset(&get_cmd, 0, sizeof(get_cmd));
		
		cnt++;
		if(cnt == 100){
			printf("\r\nMaster<-->RPU1 write and read test done !!\r\n");
			printf("\r\nMaster>RPC service exiting !!\r\n");
			/* Send shutdown signal to remote application  when you want to stop*/
			terminate_rpc_app();
			sleep(3);
			cnt = 0;
			break;
		}
	}
	#endif

	
	/* Close rpmsg_device rpmsg device */
	close(rpmsg_device->rpmsg_dev_fd);

	/* Free up resources */
	free(rpmsg_device->rpc);
	free(rpmsg_device->rpc_response);

error0:
	free(rpmsg_device);

	stop_remote();

	return ret;
}
