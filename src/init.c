#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <dirent.h>
#include <sched.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <mntent.h>
#include <sys/epoll.h>
#include <inttypes.h>
#include <ctype.h>
#include <sys/prctl.h>

#include "../config.h"
#include "hyper.h"
#include "net.h"
#include "util.h"
#include "exec.h"
#include "event.h"
#include "parse.h"
#include "container.h"
#include "syscall.h"

static struct hyper_pod global_pod = {
	.containers	=	LIST_HEAD_INIT(global_pod.containers),
	.exec_head	=	LIST_HEAD_INIT(global_pod.exec_head),
};

#define MAXEVENTS	10

struct hyper_epoll hyper_epoll;

sigset_t orig_mask;

static int hyper_handle_exit(struct hyper_pod *pod);

static int hyper_set_win_size(struct hyper_pod *pod, char *json, int length)
{
	struct winsize size;
	struct hyper_exec *exec;
	int ret = -1;

	fprintf(stdout, "call hyper_set_win_size, json %s, len %d\n", json, length);
	JSON_Value *value = hyper_json_parse(json, length);
	if (value == NULL) {
		fprintf(stderr, "set term size failed\n");
		goto out;
	}
	const char *container = json_object_get_string(json_object(value), "container");
	const char *process = json_object_get_string(json_object(value), "process");
	if (!container || !process) {
		fprintf(stderr, "call hyper_set_win_size, invalid config");
		goto out;
	}

	exec = hyper_find_process(pod, container, process);
	if (!exec) {
		fprintf(stderr, "call hyper_set_win_size, can not find the process: %s\n", process);
		goto out;
	}

	size.ws_row = (int)json_object_get_number(json_object(value), "row");
	size.ws_col = (int)json_object_get_number(json_object(value), "column");

	ret = ioctl(exec->ptyfd, TIOCSWINSZ, &size);
	if (ret < 0)
		perror("cannot ioctl to set pty device term size");

out:
	json_value_free(value);
	return ret;
}

static void hyper_kill_process(int pid)
{
	char path[64];
	char *line = NULL, *ignore = "SigIgn:";
	size_t len = 0;
	ssize_t read;
	FILE *file;
	char *sub;

	sprintf(path, "/proc/%u/status", pid);

	fprintf(stdout, "fopen %s\n", path);
	file = fopen(path, "r");
	if (file == NULL) {
		perror("can not open process proc status file");
		return;
	}

	while ((read = getline(&line, &len, file)) != -1) {
		long mask;

		if (strstr(line, ignore) == NULL)
			continue;

		sub = line + strlen(ignore);
		fprintf(stdout, "find sigign %s", sub);

		mask = atol(sub);
		fprintf(stdout, "mask is %ld\n", mask);

		if ((mask >> (SIGTERM - 1)) & 0x1) {
			fprintf(stdout, "signal term is ignored, kill it\n");
			kill(pid, SIGKILL);
		}

		break;
	}

	fclose(file);
	free(line);
}

static void hyper_term_all(struct hyper_pod *pod)
{
	int npids = 0;
	int index = 0;
	int pid;
	DIR *dp;
	struct dirent *de;
	pid_t *pids = NULL;
	struct hyper_exec *e;

	dp = opendir("/proc");
	if (dp == NULL)
		return;

	while ((de = readdir(dp)) && de != NULL) {
		if (!isdigit(de->d_name[0]))
			continue;
		pid = atoi(de->d_name);
		if (pid == 1)
			continue;
		if (index <= npids) {
			pids = realloc(pids, npids + 16384);
			if (pids == NULL)
				return;
			npids += 16384;
		}

		pids[index++] = pid;
	}

	fprintf(stdout, "Sending SIGTERM\n");

	for (--index; index >= 0; --index) {
		kill(pids[index], SIGTERM);
	}

	free(pids);
	closedir(dp);

	list_for_each_entry(e, &pod->exec_head, list)
		hyper_kill_process(e->pid);
}

static int hyper_handle_exit(struct hyper_pod *pod)
{
	int pid, status;
	/* pid + exit code */
	uint8_t data[5];

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		data[4] = 0;

		if (WIFEXITED(status)) {
			data[4] = WEXITSTATUS(status);
			fprintf(stdout, "pid %d exit normally, status %" PRIu8 "\n",
				pid, data[4]);

		} else if (WIFSIGNALED(status)) {
			fprintf(stdout, "pid %d exit by signal, status %d\n",
				pid, WTERMSIG(status));
		}

		if (pod && hyper_handle_exec_exit(pod, pid, data[4]) < 0)
			fprintf(stderr, "signal_loop send eof failed\n");
	}

	return 0;
}

static void pod_init_sigchld(int sig)
{
	hyper_handle_exit(NULL);
}

static void hyper_init_sigchld(int sig)
{
	hyper_handle_exit(&global_pod);
}

struct hyper_pod_arg {
	struct hyper_pod	*pod;
	int		ctl_pipe[2];
};

static int hyper_pod_init(void *data)
{
	struct hyper_pod_arg *arg = data;
	struct hyper_pod *pod = arg->pod;
	sigset_t mask;

	close(arg->ctl_pipe[0]);
	close(hyper_epoll.efd);
	close(hyper_epoll.ctl.fd);
	close(hyper_epoll.tty.fd);

	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	if (sigprocmask(SIG_UNBLOCK, &mask, NULL) < 0) {
		perror("sigprocmask SIGCHLD failed");
		return -1;
	}
	signal(SIGCHLD, pod_init_sigchld);

	/* mount new proc directory */
	if (umount("/proc") < 0) {
		perror("umount proc filesystem failed\n");
		goto fail;
	}

	if (mount("proc", "/proc", "proc", MS_NOSUID| MS_NODEV| MS_NOEXEC, NULL) < 0) {
		perror("mount proc filesystem failed\n");
		goto fail;
	}

	if (sethostname(pod->hostname, strlen(pod->hostname)) < 0) {
		perror("set host name failed");
		goto fail;
	}

	if (hyper_send_type(arg->ctl_pipe[1], READY) < 0) {
		fprintf(stderr, "pod init send ready message failed\n");
		goto fail;
	}

	close(arg->ctl_pipe[1]);

	for (;;)
		pause(); /* infinite loop and handle SIGCHLD */
out:
	_exit(-1);

fail:
	hyper_send_type(arg->ctl_pipe[1], ERROR);
	close(arg->ctl_pipe[1]);

	goto out;
}

static int hyper_start_containers(struct hyper_pod *pod)
{
	struct hyper_container *c;

	// TODO: setup containers and run container init processes
	//       via separated hyperstart APIs
	list_for_each_entry(c, &pod->containers, list) {
		if (hyper_setup_container(c, pod) < 0)
			return -1;
		if (hyper_run_process(&c->exec) < 0)
			return -1;
		pod->remains++;
	}

	return 0;
}

static int hyper_setup_pod_init(struct hyper_pod *pod)
{
	int stacksize = getpagesize() * 4;
	int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWIPC |
		    CLONE_NEWUTS;

	struct hyper_pod_arg arg = {
		.pod		= NULL,
		.ctl_pipe	= {-1, -1},
	};

	uint32_t type;
	void *stack;
	int ret = -1, init_pid;

	if (pipe2(arg.ctl_pipe, O_CLOEXEC) < 0) {
		perror("create pipe between hyper init and pod init failed");
		goto out;
	}

	stack = malloc(stacksize);
	if (stack == NULL) {
		perror("fail to allocate stack for pod init");
		goto out;
	}

	arg.pod = pod;

	init_pid = clone(hyper_pod_init, stack + stacksize, flags, &arg);
	free(stack);
	if (init_pid < 0) {
		perror("create pod init process failed");
		goto out;
	}
	fprintf(stdout, "pod init pid %d\n", init_pid);

	/* Wait for pod init start */
	if (hyper_get_type(arg.ctl_pipe[0], &type) < 0) {
		perror("get pod init ready message failed");
		goto out;
	}

	if (type != READY) {
		fprintf(stderr, "get incorrect message type %d, expect READY\n", type);
		goto out;
	}

	pod->init_pid = init_pid;
	ret = 0;
out:
	close(arg.ctl_pipe[1]);
	close(arg.ctl_pipe[0]);
	return ret;
}

// enter the sanbox and pass to the child, shouldn't call from the init process
int hyper_enter_sandbox(struct hyper_pod *pod, int pidpipe)
{
	int ret = -1, pidns = -1, utsns = -1, ipcns = -1;
	char path[512];

	sprintf(path, "/proc/%d/ns/pid", pod->init_pid);
	pidns = open(path, O_RDONLY| O_CLOEXEC);
	if (pidns < 0) {
		perror("fail to open pidns of pod init");
		goto out;
	}

	sprintf(path, "/proc/%d/ns/uts", pod->init_pid);
	utsns = open(path, O_RDONLY| O_CLOEXEC);
	if (utsns < 0) {
		perror("fail to open utsns of pod init");
		goto out;
	}

	sprintf(path, "/proc/%d/ns/ipc", pod->init_pid);
	ipcns = open(path, O_RDONLY| O_CLOEXEC);
	if (ipcns < 0) {
		perror("fail to open ipcns of pod init");
		goto out;
	}

	if (setns(pidns, CLONE_NEWPID) < 0 ||
	    setns(utsns, CLONE_NEWUTS) < 0 ||
	    setns(ipcns, CLONE_NEWIPC) < 0) {
		perror("fail to enter the sandbox");
		goto out;
	}

	/* current process isn't in the pidns even setns(pidns, CLONE_NEWPID)
	 * was called. fork() is needed, so that the child process will run in
	 * the pidns, see man 2 setns */
	ret = fork();
	if (ret < 0) {
		perror("fail to fork");
		goto out;
	} else if (ret > 0) {
		fprintf(stdout, "create child process pid=%d in the sandbox\n", ret);
		if (pidpipe > 0) {
			hyper_send_type(pidpipe, ret);
		}
		_exit(0);
	}

out:
	close(pidns);
	close(ipcns);
	close(utsns);

	return ret;
}

#ifdef WITH_VBOX

#define MAX_HOST_NAME  256
#define MAX_NLS_NAME    32

#define VBSF_MOUNT_SIGNATURE_BYTE_0 '\377'
#define VBSF_MOUNT_SIGNATURE_BYTE_1 '\376'
#define VBSF_MOUNT_SIGNATURE_BYTE_2 '\375'

struct vbsf_mount_info_new
{
	char nullchar;			/* name cannot be '\0' -- we use this field
					 to distinguish between the old structure
					 and the new structure */
	char signature[3];		/* signature */
	int  length;			/* length of the whole structure */
	char name[MAX_HOST_NAME];	/* share name */
	char nls_name[MAX_NLS_NAME];	/* name of an I/O charset */
	int  uid;			/* user ID for all entries, default 0=root */
	int  gid;			/* group ID for all entries, default 0=root */
	int  ttl;			/* time to live */
	int  dmode;			/* mode for directories if != 0xffffffff */
	int  fmode;			/* mode for regular files if != 0xffffffff */
	int  dmask;			/* umask applied to directories */
	int  fmask;			/* umask applied to regular files */
};

static int hyper_setup_shared(struct hyper_pod *pod)
{
	struct vbsf_mount_info_new mntinf;

	if (pod->share_tag == NULL) {
		fprintf(stdout, "no shared directory\n");
		return 0;
	}

	if (hyper_mkdir(SHARED_DIR, 0755) < 0) {
		perror("fail to create " SHARED_DIR);
		return -1;
	}

	bzero(&mntinf, sizeof(mntinf));
	mntinf.nullchar = '\0';
	mntinf.signature[0]	= VBSF_MOUNT_SIGNATURE_BYTE_0;
	mntinf.signature[1]	= VBSF_MOUNT_SIGNATURE_BYTE_1;
	mntinf.signature[2]	= VBSF_MOUNT_SIGNATURE_BYTE_2;
	mntinf.length		= sizeof(mntinf);
	mntinf.dmode		= ~0U;
	mntinf.fmode		= ~0U;
	strcpy(mntinf.name, pod->share_tag);

	if (mount(NULL, SHARED_DIR, "vboxsf",
		  MS_NODEV, &mntinf) < 0) {
		perror("fail to mount shared dir");
		return -1;
	}

	return 0;
}
#else
static int hyper_setup_shared(struct hyper_pod *pod)
{
	if (pod->share_tag == NULL) {
		fprintf(stdout, "no shared directory\n");
		return 0;
	}

	if (hyper_mkdir(SHARED_DIR, 0755) < 0) {
		perror("fail to create " SHARED_DIR);
		return -1;
	}

	if (mount(pod->share_tag, SHARED_DIR, "9p",
		  MS_MGC_VAL| MS_NODEV, "trans=virtio") < 0) {

		perror("fail to mount shared dir");
		return -1;
	}

	return 0;
}
#endif

static int hyper_setup_virtual_hyperstart_exec_container(struct hyper_pod *pod)
{
	if (hyper_mkdir("/tmp/hyper/" HYPERSTART_EXEC_CONTAINER, 0755) < 0) {
		perror("create virtual hyperstart-exec container directory failed");
		return -1;
	}

	// for creating ptymaster when adding process with terminal=true
	if (symlink("/dev/pts", "/tmp/hyper/" HYPERSTART_EXEC_CONTAINER "/devpts") < 0) {
		perror("create virtual hyperstart-exec container's /dev symlink failed");
		return -1;
	}

	return 0;
}

static int hyper_setup_pod(struct hyper_pod *pod)
{
	/* create sandbox directory */
	if (hyper_mkdir("/tmp/hyper", 0755) < 0) {
		perror("create sandbox directory failed");
		return -1;
	}

	if (hyper_setup_network(pod) < 0) {
		fprintf(stderr, "setup network failed\n");
		return -1;
	}

	if (hyper_setup_dns(pod) < 0) {
		fprintf(stderr, "setup network failed\n");
		return -1;
	}

	if (hyper_setup_shared(pod) < 0) {
		fprintf(stderr, "setup shared directory failed\n");
		return -1;
	}

	if (hyper_setup_portmapping(pod) < 0) {
		fprintf(stderr, "setup port mapping failed\n");
		return -1;
	}

	if (hyper_setup_pod_init(pod) < 0) {
		fprintf(stderr, "start container failed\n");
		return -1;
	}

	if (hyper_setup_virtual_hyperstart_exec_container(pod) < 0) {
		return -1;
	}

	return 0;
}

static void hyper_print_uptime(void)
{
	char buf[128];
	int fd = open("/proc/uptime", O_RDONLY);

	if (fd < 0)
		return;
	memset(buf, 0, sizeof(buf));
	if (read(fd, buf, sizeof(buf)))
		fprintf(stdout, "uptime %s\n", buf);

	close(fd);
}

static void hyper_flush_channel()
{
	// Todo: remove this after we implement DESTROYVM message.
	struct hyper_buf *ctl_buf = &hyper_epoll.ctl.wbuf;
	struct hyper_buf *tty_buf = &hyper_epoll.tty.wbuf;

	hyper_send_data_block(hyper_epoll.ctl.fd, ctl_buf->data, ctl_buf->get);
	hyper_send_data_block(hyper_epoll.tty.fd, tty_buf->data, tty_buf->get);
}

void hyper_pod_destroyed(int failed)
{
	hyper_ctl_append_msg(&hyper_epoll.ctl, failed?ERROR:ACK, NULL, 0);
	// Todo: this doesn't make sure peer receives the data
	hyper_flush_channel();
	// Todo: don't shutdown vm until hyperstart receives the DESTROYVM message,
	// peer will send to DESTROYVM until receives the whole data of tty/ctl.
	hyper_shutdown();
}

static int hyper_destroy_pod(struct hyper_pod *pod, int error)
{
	if (pod->init_pid == 0 || pod->remains == 0) {
		/* Pod stopped, just shutdown */
		hyper_pod_destroyed(error);
	} else {
		/* Kill pod */
		hyper_term_all(pod);
	}
	return 0;
}

static int hyper_start_pod(struct hyper_pod *pod, char *json, int length)
{
	fprintf(stdout, "call hyper_start_pod, json %s, len %d\n", json, length);

	if (pod->init_pid)
		fprintf(stdout, "pod init_pid exist %d\n", pod->init_pid);

	hyper_sync_time_hctosys();
	if (hyper_parse_pod(pod, json, length) < 0) {
		fprintf(stderr, "parse pod json failed\n");
		return -1;
	}

	if (hyper_setup_pod(pod) < 0) {
		hyper_destroy_pod(pod, 1);
		return -1;
	}

	if (hyper_start_containers(pod) < 0) {
		fprintf(stderr, "start containers failed\n");
		hyper_destroy_pod(pod, 1);
		return -1;
	}

	return 0;
}

static int hyper_new_container(struct hyper_pod *pod, char *json, int length)
{
	int ret;
	struct hyper_container *c;

	fprintf(stdout, "call hyper_new_container, json %s, len %d\n", json, length);

	if (!pod->init_pid) {
		fprintf(stdout, "the pod is not created yet\n");
		return -1;
	}

	c = hyper_parse_new_container(pod, json, length);
	if (c == NULL) {
		fprintf(stderr, "parse container json failed\n");
		return -1;
	}

	if (hyper_has_container(pod, c->id)) {
		fprintf(stderr, "container id conflicts");
		hyper_cleanup_container(c, pod);
		return -1;
	}

	list_add_tail(&c->list, &pod->containers);
	ret = hyper_setup_container(c, pod);
	if (ret >= 0)
		ret = hyper_run_process(&c->exec);

	if (ret < 0) {
		//TODO full grace cleanup
		hyper_cleanup_container(c, pod);
	} else {
		pod->remains++;
	}

	return ret;
}

static int hyper_kill_container(struct hyper_pod *pod, char *json, int length)
{
	struct hyper_container *c;
	int ret = -1;

	JSON_Value *value = hyper_json_parse(json, length);
	if (value == NULL) {
		goto out;
	}

	const char *id = json_object_get_string(json_object(value), "container");
	c = hyper_find_container(pod, id);
	if (c == NULL) {
		fprintf(stderr, "can not find container whose id is %s\n", id);
		goto out;
	}

	kill(c->exec.pid, (int)json_object_get_number(json_object(value), "signal"));
	ret = 0;
out:
	json_value_free(value);
	return ret;
}

static int hyper_signal_process(struct hyper_pod *pod, char *json, int length)
{
	struct hyper_exec *exec;
	int ret = -1;

	JSON_Value *value = hyper_json_parse(json, length);
	if (value == NULL) {
		goto out;
	}

	const char *container = json_object_get_string(json_object(value), "container");
	const char *process = json_object_get_string(json_object(value), "process");
	exec = hyper_find_process(pod, container, process);
	if (exec == NULL) {
		fprintf(stderr, "can not find process");
		goto out;
	}

	kill(exec->pid, (int)json_object_get_number(json_object(value), "signal"));
	ret = 0;
out:
	json_value_free(value);
	return ret;
}

static int hyper_remove_container(struct hyper_pod *pod, char *json, int length)
{
	struct hyper_container *c;
	int ret = -1;

	JSON_Value *value = hyper_json_parse(json, length);
	if (value == NULL) {
		goto out;
	}

	const char *id = json_object_get_string(json_object(value), "container");
	c = hyper_find_container(pod, id);
	if (c == NULL) {
		fprintf(stderr, "can not find container whose id is %s\n", id);
		goto out;
	}

	if (c->exec.exit != 1) {
		fprintf(stderr, "container %s has not been stopped\n", id);
		goto out;
	}

	hyper_cleanup_container(c, pod);

	ret = 0;
out:
	json_value_free(value);
	return ret;
}

struct hyper_file_arg {
	int 		rw;
	int 		mntns;
	int 		pipe[2];
	char 		*file;
};

static int hyper_open_container_file(void *data)
{
	struct hyper_file_arg *arg = data;
	int fd = -1, ret = -1, size;

	if (setns(arg->mntns, CLONE_NEWNS) < 0) {
		perror("fail to enter container ns");
		goto exit;
	}

	if (arg->rw == WRITEFILE) {
		fd = open(arg->file, O_CREAT| O_TRUNC| O_WRONLY, 0644);
	} else {
		fd = open(arg->file, O_RDONLY);
	}
	if (fd < 0) {
		perror("fail to open target file");
		goto exit;
	}
	ret = 0;

exit:
	size = write(arg->pipe[1], &fd, sizeof(fd));
	if (size != sizeof(fd) && ret == 0) {
		ret = -1;
	}
	exit(ret);
}

static int hyper_cmd_rw_file(struct hyper_pod *pod, char *json, int length, uint32_t *rdatalen, uint8_t **rdata, int rw)
{
	struct file_command cmd = {
		.id = NULL,
		.file = NULL,
	};
	struct hyper_file_arg arg = {
		.pipe = {-1, -1},
		.rw = rw,
	};
	struct hyper_container *c;
	char *data = NULL;
	void *stack = NULL;
	int stacksize = getpagesize() * 4;
	int fd = -1, len = 0, ret = -1, datalen = 0;
	int pid, size;

	fprintf(stdout, "%s: %s\n", __func__, rw == WRITEFILE ? "write" : "read");

	if (rw == WRITEFILE) {
		// TODO: send the data via hyperstream rather than append it at the end of the command	
		data = strchr(json, '}');
		if (data == NULL) {
			goto out;
		}
		data++;
		datalen = length - (data - json);
		length = data - json;
	}

	if (hyper_parse_file_command(&cmd, json, length) < 0) {
		goto out;
	}
	arg.file = cmd.file;

	c = hyper_find_container(pod, cmd.id);
	if (c == NULL) {
		fprintf(stderr, "can not find container whose id is %s\n", cmd.id);
		goto out;
	}

	arg.mntns = c->ns;
	if (arg.mntns < 0) {
		perror("fail to open mnt ns");
		goto out;
	}

	if (pipe2(arg.pipe, O_CLOEXEC) < 0) {
		perror("create pipe failed");
		goto out;
	}

	stack = malloc(stacksize);
	if (stack == NULL) {
		perror("fail to allocate stack for container init");
		goto out;
	}

	pid = clone(hyper_open_container_file, stack + stacksize, CLONE_FILES | SIGCHLD, &arg);
	if (pid < 0) {
		perror("fail to fork child process");
		goto out;
	}

	size = read(arg.pipe[0], &fd, sizeof(fd));
	if (size != sizeof(fd)) {
		perror("fail to read fd from pipe");
		goto out;
	}
	if (fd < 0) {
		perror("child open target file failed");
		goto out;
	}

	if (rw == READFILE) {
		struct stat st;
		if(fstat(fd, &st) < 0) {
			perror("fail to state file");
			goto out;
		}
		*rdatalen = datalen = st.st_size;
		data = malloc(datalen);
		*rdata = (uint8_t *) data;
		if (*rdata == NULL) {
			fprintf(stderr, "allocate memory for reading file failed\n");
			goto out;
		}
		fprintf(stdout, "file length %d\n", *rdatalen);
	}

	while(len < datalen) {
		if (rw == WRITEFILE) {
			size = write(fd, data + len, datalen - len);
		} else {
			size = read(fd, data + len, datalen - len);
		}

		if (size < 0) {
			if (errno == EINTR)
				continue;

			perror("fail to operate data to file");
			goto out;
		}

		len += size;
	}
	ret = 0;

out:
	close(fd);
	close(arg.pipe[0]);
	close(arg.pipe[1]);
	free(cmd.id);
	free(cmd.file);
	free(stack);

	return ret;
}

static void hyper_cmd_online_cpu_mem()
{
	int pid = fork();
	if (pid < 0) {
		perror("fail to fork online process");
	} else if (pid == 0) {
		online_cpu();
		online_memory();
		exit(0);
	}
}

static int hyper_setup_ctl_channel(char *name)
{
	uint8_t buf[8];
	int ret = hyper_open_channel(name, 0);
	if (ret < 0)
		return ret;

	fprintf(stdout, "send ready message\n");
	hyper_set_be32(buf, READY);
	hyper_set_be32(buf + 4, 8);
	if (hyper_send_data_block(ret, buf, 8) < 0) {
		perror("send READY MESSAGE failed\n");
		goto out;
	}

	return ret;
out:
	close(ret);
	return -1;
}

static int hyper_setup_tty_channel(char *name)
{
	int ret = hyper_open_channel(name, O_NONBLOCK);
	if (ret < 0)
		return -1;

	return ret;
}

static int hyper_ttyfd_handle(struct hyper_event *de, uint32_t len)
{
	struct hyper_buf *rbuf = &de->rbuf;
	struct hyper_pod *pod = de->ptr;
	struct hyper_exec *exec;
	struct hyper_buf *wbuf;
	uint64_t seq = 0;
	int size;

	seq = hyper_get_be64(rbuf->data);

	fprintf(stdout, "\n%s seq %" PRIu64", len %" PRIu32"\n", __func__, seq, len - 12);

	exec = hyper_find_exec_by_seq(pod, seq);
	if (exec == NULL) {
		wbuf = &de->wbuf;
		fprintf(stderr, "can't find exec whose seq is %" PRIu64 "\n", seq);

		/* goodbye */
		if (wbuf->get + 12 > wbuf->size)
			return 0;

		hyper_set_be64(wbuf->data + wbuf->get, seq);
		hyper_set_be32(wbuf->data + wbuf->get + 8, 12);
		wbuf->get += 12;

		if (hyper_modify_event(hyper_epoll.efd, de, EPOLLIN | EPOLLOUT) < 0) {
			fprintf(stderr, "modify ctl tty event to in & out failed\n");
			return -1;
		}

		return 0;
	}

	fprintf(stdout, "find exec %s pid %d, seq is %" PRIu64 "\n",
		exec->container_id ? exec->container_id : "pod", exec->pid, exec->seq);
	// if exec is exited or stdin is closed by process, the event fd of exec is invalid.
	// don't accept any input.
	if (exec->exit || exec->close_stdin_request || exec->stdinev.fd < 0) {
		fprintf(stdout, "exec seq %" PRIu64 " exited, don't accept any input\n", exec->seq);
		return 0;
	}

	size = len - STREAM_HEADER_SIZE;
	/* size == 0 means we had received eof */
	if (size == 0 && !exec->tty) {
		exec->close_stdin_request = 1;
		fprintf(stdout, "get close stdin request\n");
		/* we can't hup the stdinev here, force hup on next write */
		if (hyper_modify_event(hyper_epoll.efd, &exec->stdinev, EPOLLOUT) < 0) {
			fprintf(stderr, "modify exec pts event to in & out failed\n");
			return -1;
		}
	}

	wbuf = &exec->stdinev.wbuf;
	if (size > (wbuf->size - wbuf->get)) {
		/* buffer is full, discard the data */
		/* TODO: properly handle the discard data */
		size = wbuf->size - wbuf->get;
	}
	if (size > 0) {
		memcpy(wbuf->data + wbuf->get, rbuf->data + 12, size);
		wbuf->get += size;
		if (hyper_modify_event(hyper_epoll.efd, &exec->stdinev, EPOLLOUT) < 0) {
			fprintf(stderr, "modify exec pts event to in & out failed\n");
			return -1;
		}
	}

	return 0;
}

static ssize_t hyper_channel_read(struct hyper_event *he, int efd, int len, int events)
{
	struct hyper_buf *buf = &he->rbuf;
	ssize_t size;

	size = nonblock_read(he->fd, buf->data + buf->get, len);
	if (size < 0)
		goto out;

	// check if peer is dissapeared
	if ((size == 0) && (events & EPOLLHUP)) {
		he->hup = 1;
		fprintf(stdout, "peer is disappeared\n");
		// use EPOLLOUT| EPOLLET event to check if peer disappeared
		hyper_modify_event(efd, he, EPOLLOUT| EPOLLET);
	}

out:
	return size;
}

static int hyper_ttyfd_read(struct hyper_event *he, int efd, int events)
{
	struct hyper_buf *buf = &he->rbuf;
	uint32_t len;
	int size;
	int ret;

	if (buf->get < STREAM_HEADER_SIZE) {
		size = hyper_channel_read(he, efd, STREAM_HEADER_SIZE - buf->get, events);
		if (size < 0) {
			return size;
		}

		buf->get += size;
		if (buf->get < STREAM_HEADER_SIZE) {
			return 0;
		}
	}

	len = hyper_get_be32(buf->data + STREAM_HEADER_LENGTH_OFFSET);
	fprintf(stdout, "%s: get length %" PRIu32"\n", __func__, len);
	if (len > buf->size) {
		fprintf(stderr, "get length %" PRIu32", too long\n", len);
		return -1;
	}

	size = hyper_channel_read(he, efd, len - buf->get, events);
	if (size < 0) {
		return size;
	}
	buf->get += size;
	if (buf->get < len) {
		return 0;
	}

	/* get and consume the whole data */
	ret = hyper_ttyfd_handle(he, len);
	buf->get = 0;

	return ret == 0 ? 0 : -1;
}

int hyper_ctl_append_msg(struct hyper_event *he, uint32_t type, uint8_t *data, uint32_t len)
{
	int ret = -1;
	fprintf(stdout, "hyper ctl append type %d, len %d\n", type, len);

	uint8_t *new_data = realloc(data, len + 8);
	if (new_data == NULL) {
		new_data = data;
		goto out;
	}

	memmove(new_data + 8, new_data, len);
	hyper_set_be32(new_data, type);
	hyper_set_be32(new_data + 4, len + 8);

	ret = hyper_wbuf_append_msg(he, new_data, len + 8);
out:
	free(new_data);
	return ret;
}

static int hyper_ctlmsg_handle(struct hyper_event *he, uint32_t len)
{
	struct hyper_buf *buf = &he->rbuf;
	struct hyper_pod *pod = he->ptr;
	uint32_t type = 0, datalen = 0;
	uint8_t *data = NULL;
	int ret = 0;

	// append a null byte to it. hyper_ctlfd_read() left this room for us.
	buf->data[buf->get] = 0;

	type = hyper_get_be32(buf->data);

	fprintf(stdout, "%s, type %" PRIu32 ", len %" PRIu32 "\n",
		__func__, type, len);

	switch (type) {
	case GETVERSION:
		data = malloc(4);
		datalen = 4;
		hyper_set_be32(data, APIVERSION);
		break;
	case STARTPOD:
		ret = hyper_start_pod(pod, (char *)buf->data + 8, len - 8);
		hyper_print_uptime();
		break;
	case DESTROYPOD:
		pod->req_destroy = 1;
		fprintf(stdout, "get DESTROYPOD message\n");
		hyper_destroy_pod(pod, 0);
		return 0;
	case EXECCMD:
		ret = hyper_exec_cmd(pod, (char *)buf->data + 8, len - 8);
		break;
	case WRITEFILE:
		ret = hyper_cmd_rw_file(pod, (char *)buf->data + 8, len - 8, NULL, NULL, WRITEFILE);
		break;
	case READFILE:
		ret = hyper_cmd_rw_file(pod, (char *)buf->data + 8, len - 8, &datalen, &data, READFILE);
		break;
	case PING:
		break;
	case READY:
		ret = hyper_rescan();
		break;
	case WINSIZE:
		ret = hyper_set_win_size(pod, (char *)buf->data + 8, len - 8);
		break;
	case NEWCONTAINER:
		ret = hyper_new_container(pod, (char *)buf->data + 8, len - 8);
		break;
	case KILLCONTAINER:
		ret = hyper_kill_container(pod, (char *)buf->data + 8, len - 8);
		break;
	case REMOVECONTAINER:
		ret = hyper_remove_container(pod, (char *)buf->data + 8, len - 8);
		break;
	case ONLINECPUMEM:
		hyper_cmd_online_cpu_mem();
		break;
	case SETUPINTERFACE:
		ret = hyper_cmd_setup_interface((char *)buf->data + 8, len - 8);
		break;
	case SETUPROUTE:
		ret = hyper_cmd_setup_route((char *)buf->data + 8, len - 8);
		break;
	case SIGNALPROCESS:
		ret = hyper_signal_process(pod, (char *)buf->data + 8, len - 8);
		break;
	case GETPOD_DEPRECATED:
	case STOPPOD_DEPRECATED:
	case RESTARTCONTAINER_DEPRECATED:
	case CMDFINISHED_DEPRECATED:
	case PODFINISHED_DEPRECATED:
		fprintf(stderr, "get abandoned command\n");
		ret = -1;
		break;
	default:
		ret = -1;
		break;
	}

	return hyper_ctl_append_msg(he, ret < 0 ? ERROR: ACK, data, datalen);
}

static int hyper_ctlfd_read(struct hyper_event *he, int efd, int events)
{
	struct hyper_buf *buf = &he->rbuf;
	uint32_t len;
	int size;
	int ret;

	if (buf->get < CONTROL_HEADER_SIZE) {
		size = hyper_channel_read(he, efd, CONTROL_HEADER_SIZE - buf->get, events);
		if (size < 0) {
			return size;
		}
		if (size > 0) {
			uint8_t *data = malloc(4);
			/* control channel, need ack */
			hyper_set_be32(data, size);
			hyper_ctl_append_msg(&hyper_epoll.ctl, NEXT, data, 4);
		}
		buf->get += size;
		if (buf->get < CONTROL_HEADER_SIZE) {
			return 0;
		}
	}

	len = hyper_get_be32(buf->data + CONTROL_HEADER_LENGTH_OFFSET);
	fprintf(stdout, "%s: get length %" PRIu32"\n", __func__, len);
	// test it with '>=' to leave at least one byte in hyper_ctlfd_handle(),
	// so that hyper_ctlfd_handle() can convert the data to c-string inplace.
	if (len >= buf->size) {
		uint8_t *new_data;
		fprintf(stderr, "get length %" PRIu32", too long, extend buffer\n", len);
		new_data = realloc(buf->data, len + 1);
		if (!new_data) {
			perror("realloc channel read buffer failed");
			return -1;
		}
		buf->data = new_data;
		buf->size = len + 1;
	}

	size = hyper_channel_read(he, efd, len - buf->get, events);
	if (size < 0) {
		return size;
	}
	if (size > 0) {
		uint8_t *data = malloc(4);
		/* control channel, need ack */
		hyper_set_be32(data, size);
		hyper_ctl_append_msg(&hyper_epoll.ctl, NEXT, data, 4);
	}
	buf->get += size;
	if (buf->get < len) {
		return 0;
	}

	/* get and consume the whole data */
	ret = hyper_ctlmsg_handle(he, len);
	buf->get = 0;

	return ret == 0 ? 0 : -1;
}

static int hyper_channel_write(struct hyper_event *he, int efd, int events)
{
	// virtio serial port receives writable event, it means peer appears
	if (he->hup){
		he->hup = 0;
		fprintf(stdout, "peer is appeared\n");
		hyper_modify_event(efd, he, EPOLLIN| EPOLLOUT);
	}

	return hyper_event_write(he, efd, events);
}

static struct hyper_event_ops hyper_ctlfd_ops = {
	.read		= hyper_ctlfd_read,
	.write		= hyper_channel_write,
	.rbuf_size	= 10240,
	.wbuf_size	= 4096,
};

static struct hyper_event_ops hyper_ttyfd_ops = {
	.read		= hyper_ttyfd_read,
	.write		= hyper_channel_write,
	.rbuf_size	= 4096,
	.wbuf_size	= 10240,
};

static int hyper_loop(void)
{
	int i, n;
	struct epoll_event *events;
	struct hyper_pod *pod = &global_pod;
	sigset_t mask, omask;
	struct rlimit limit;
	char *filemax = "1000000";

	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);

	/*
	 * block SIGCHLD in the loop except when in the syscall of
	 * epoll_pwait(), it ensures that the SIGCHLD handling and
	 * the events handling are exclusive.
	 */
	if (sigprocmask(SIG_BLOCK, &mask, &omask) < 0) {
		perror("sigprocmask SIGCHLD failed");
		return -1;
	}
	// need original mask to restore sigmask of child processes
	orig_mask = omask;
	sigdelset(&omask, SIGCHLD);
	signal(SIGCHLD, hyper_init_sigchld);
	prctl(PR_SET_CHILD_SUBREAPER, 1);

	if (hyper_write_file("/proc/sys/fs/file-max", filemax, strlen(filemax)) < 0) {
		fprintf(stderr, "sysctl: setup default file-max(%s) failed\n", filemax);
		return -1;
	}

	// setup open file limit
	limit.rlim_cur = limit.rlim_max = atoi(filemax);
	if (setrlimit(RLIMIT_NOFILE, &limit) < 0) {
		perror("set rlimit for NOFILE failed");
		return -1;
	}

	// setup process num limit
	limit.rlim_cur = limit.rlim_max = 30604;
	if (setrlimit(RLIMIT_NPROC, &limit) < 0) {
		perror("set rlimit for NPROC failed");
		return -1;
	}

	// setup pending signal limit, same with NRPROC
	if (setrlimit(RLIMIT_SIGPENDING, &limit) < 0) {
		perror("set rlimit for SIGPENDING failed");
		return -1;
	}

	hyper_epoll.efd = epoll_create1(EPOLL_CLOEXEC);
	if (hyper_epoll.efd < 0) {
		perror("epoll_create failed");
		return -1;
	}

	fprintf(stdout, "hyper_init_event hyper ctlfd event %p, ops %p, fd %d\n",
		&hyper_epoll.ctl, &hyper_ctlfd_ops, hyper_epoll.ctl.fd);
	if (hyper_init_event(&hyper_epoll.ctl, &hyper_ctlfd_ops, pod) < 0 ||
	    hyper_add_event(hyper_epoll.efd, &hyper_epoll.ctl, EPOLLIN) < 0) {
		return -1;
	}

	fprintf(stdout, "hyper_init_event hyper ttyfd event %p, ops %p, fd %d\n",
		&hyper_epoll.tty, &hyper_ttyfd_ops, hyper_epoll.tty.fd);
	if (hyper_init_event(&hyper_epoll.tty, &hyper_ttyfd_ops, pod) < 0 ||
	    hyper_add_event(hyper_epoll.efd, &hyper_epoll.tty, EPOLLIN) < 0) {
		return -1;
	}

	events = calloc(MAXEVENTS, sizeof(*events));

	while (1) {
		n = epoll_pwait(hyper_epoll.efd, events, MAXEVENTS, -1, &omask);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("hyper wait event failed");
			return -1;
		}

		for (i = 0; i < n; i++) {
			if (hyper_handle_event(hyper_epoll.efd, &events[i]) < 0)
				return -1;
		}
	}

	free(events);
	close(hyper_epoll.efd);
	return 0;
}

int main(int argc, char *argv[])
{
	char *cmdline, *ctl_serial, *tty_serial;

	if (mount("proc", "/proc", "proc", MS_NOSUID| MS_NODEV| MS_NOEXEC, NULL) == -1) {
		perror("mount proc failed");
		return -1;
	}

	hyper_print_uptime();

	if (mount("sysfs", "/sys", "sysfs", MS_NOSUID| MS_NODEV| MS_NOEXEC, NULL) == -1) {
		perror("mount sysfs failed");
		return -1;
	}

	if (mount("dev", "/dev", "devtmpfs", MS_NOSUID, NULL) == -1) {
		perror("mount devtmpfs failed");
		return -1;
	}

	if (hyper_mkdir("/dev/pts", 0755) < 0) {
		perror("create basic directory failed");
		return -1;
	}

	if (mount("devpts", "/dev/pts", "devpts", MS_NOSUID| MS_NOEXEC, NULL) == -1) {
		perror("mount devpts failed");
		return -1;
	}

	if (unlink("/dev/ptmx") < 0) {
		perror("remove /dev/ptmx failed");
		return -1;
	}
	if (symlink("/dev/pts/ptmx", "/dev/ptmx") < 0) {
		perror("link /dev/pts/ptmx to /dev/ptmx failed");
		return -1;
	}

	cmdline = read_cmdline();

	setsid();

	ioctl(STDIN_FILENO, TIOCSCTTY, 1);

#ifdef WITH_VBOX
	ctl_serial = "/dev/ttyS0";
	tty_serial = "/dev/ttyS1";

	if (hyper_insmod("/vboxguest.ko") < 0 ||
	    hyper_insmod("/vboxsf.ko") < 0) {
		fprintf(stderr, "fail to load modules\n");
		return -1;
	}
#else
	ctl_serial = "sh.hyper.channel.0";
	tty_serial = "sh.hyper.channel.1";
#endif

	setenv("PATH", "/bin:/sbin/:/usr/bin/:/usr/sbin/", 1);

	hyper_epoll.ctl.fd = hyper_setup_ctl_channel(ctl_serial);
	if (hyper_epoll.ctl.fd < 0) {
		fprintf(stderr, "fail to setup hyper control serial port\n");
		goto out1;
	}

	hyper_epoll.tty.fd = hyper_setup_tty_channel(tty_serial);
	if (hyper_epoll.tty.fd < 0) {
		fprintf(stderr, "fail to setup hyper tty serial port\n");
		goto out2;
	}

	hyper_loop();

	close(hyper_epoll.tty.fd);
out2:
	close(hyper_epoll.ctl.fd);
out1:
	free(cmdline);

	return 0;
}
