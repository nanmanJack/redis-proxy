#include "util.h"
#include "zmalloc.h"
#include "object.h"
#include "db.h"
#include <sys/uio.h>
#include <math.h>
#include "networking.h"

/*============================== Variable and Function Declaration =========================*/
extern struct redisServer server;
extern struct sharedObjectsStruct shared;

int _addReplyToBuffer(redisClient *c, char *s, size_t len);

/*==================================== 基础函数 ==========================================*/
/*
 * 计算出输出缓冲区的大小
 */
size_t zmalloc_size_sds(sds s) {
	return zmalloc_size(s - sizeof(struct sdshdr));
}

/*
 * 返回object->ptr所指向的字符串对象所使用的内存数量
 */
size_t getStringObjectSdsUsedMemory(robj *o) {
	switch (o->encoding) {
	case REDIS_ENCODING_RAW: return zmalloc_size_sds(o->ptr);
	case REDIS_ENCODING_EMBSTR: return sdslen(o->ptr);
	default: return 0;
	}
}

void _addReplyStringToList(redisClient *c, char *s, size_t len) {
	robj *tail;

	if (listLength(c->reply) == 0) {
		// 为字符串创建字符串对象并追加到回复链表末尾
		robj *o = createStringObject(s, len);

		listAddNodeTail(c->reply, o);
		c->reply_bytes += getStringObjectSdsUsedMemory(o);
	}
	else {
		tail = listNodeValue(listLast(c->reply));

		// Append to this object when possible.
		if (tail->ptr != NULL && tail->encoding == REDIS_ENCODING_RAW &&
			sdslen(tail->ptr) + len <= REDIS_REPLY_CHUNK_BYTES)
		{
			c->reply_bytes -= zmalloc_size_sds(tail->ptr);
			tail = dupLastObjectIfNeeded(c->reply);
			// 将字符串拼接到一个 SDS 之后
			tail->ptr = sdscatlen(tail->ptr, s, len);
			c->reply_bytes += zmalloc_size_sds(tail->ptr);
		}
		else {
			// 为字符串创建字符串对象并追加到回复链表末尾
			robj *o = createStringObject(s, len);

			listAddNodeTail(c->reply, o);
			c->reply_bytes += getStringObjectSdsUsedMemory(o);
		}
	}
	/* 防止因为加的内容太多,导致出错,但是实际上,在这个版本的代码里,下面的函数调用没有什么用处 */
	asyncCloseClientOnOutputBufferLimitReached(c);
}

/*
 * 将 C 字符串中的内容复制到回复缓冲区
 */
void addReplyString(redisClient *c, char *s, size_t len) {
	if (prepareClientToWrite(c) != REDIS_OK) return;
	if (_addReplyToBuffer(c, s, len) != REDIS_OK)
		_addReplyStringToList(c, s, len);
}

/*
 * 添加一个 long long 为整数回复，或者 bulk 或 multi bulk 的数目
 *
 * 输出格式为 <prefix><long long><crlf>
 *
 * 例子:
 *
 * *5\r\n10086\r\n
 *
 * $5\r\n10086\r\n
 */
void addReplyLongLongWithPrefix(redisClient *c, long long ll, char prefix) {
	char buf[128];
	int len;

	/* Things like $3\r\n or *2\r\n are emitted very often by the protocol
	* so we have a few shared objects to use if the integer is small
	* like it is most of the times. */
	if (prefix == '*' && ll < REDIS_SHARED_BULKHDR_LEN) {
		// 多条批量回复
		addReply(c, shared.mbulkhdr[ll]);
		return;
	}
	else if (prefix == '$' && ll < REDIS_SHARED_BULKHDR_LEN) {
		// 批量回复
		addReply(c, shared.bulkhdr[ll]);
		return;
	}

	buf[0] = prefix;
	len = ll2string(buf + 1, sizeof(buf) - 1, ll);
	buf[len + 1] = '\r';
	buf[len + 2] = '\n';
	addReplyString(c, buf, len + 3);
}

/*
 * Create the length prefix of a bulk reply.
 * example: $2234 
 */
void addReplyBulkLen(redisClient *c, robj *obj) {
	size_t len;

	if (sdsEncodedObject(obj)) {
		len = sdslen(obj->ptr);
	}
	else {
		long n = (long)obj->ptr;
		// Compute how many bytes will take this integer as a radix 10 string
		len = 1;
		if (n < 0) {
			len++;
			n = -n;
		}
		while ((n = n / 10) != 0) {
			len++;
		}
	}

	if (len < REDIS_SHARED_BULKHDR_LEN)
		addReply(c, shared.bulkhdr[len]);
	else
		addReplyLongLongWithPrefix(c, len, '$');
}

/* Add a Redis Object as a bulk reply
 *
 * 返回一个 Redis 对象作为回复
 */
void addReplyBulk(redisClient *c, robj *obj) {
	addReplyBulkLen(c, obj);
	addReply(c, obj);
	addReply(c, shared.crlf);
}



/*
 * 回复内容复制函数
 */
void *dupClientReplyValue(void *o) {
	incrRefCount((robj*)o);
	return o;
}

/*
 * 当回复列表中的最后一个对象并非属于回复的一部分时,创建该对象的一个复制品.
 */
robj *dupLastObjectIfNeeded(list *reply) {
	robj *new, *cur;
	listNode *ln;
	ln = listLast(reply);
	cur = listNodeValue(ln);
	if (cur->refcount > 1) {
		new = dupStringObject(cur);
		decrRefCount(cur);
		listNodeValue(ln) = new;
	}
	return listNodeValue(ln);
}

static void setProtocolError(redisClient *c, int pos) { /* 如果在读入协议内容是,发现内容不符合协议,那么异步地关闭这个客户端 */
	// todo
}

/*
 * 尝试将回复添加到 c->buf 中
 */
int _addReplyToBuffer(redisClient *c, char *s, size_t len) {
	size_t available = sizeof(c->buf) - c->bufpos; // 可用的空间数

	// 回复链表中已经有内容了,再添加内容到c->buf中就是错误了.
	if (listLength(c->reply) > 0) return REDIS_ERR;

	if (len > available) return REDIS_ERR; // 必须要有充足的空间

	memcpy(c->buf + c->bufpos, s, len);
	c->bufpos += len;
	return REDIS_OK;
	
}

/*
 * 将回复对象(一个SDS)添加到c->reply回复链表中
 */
void _addReplyObjectToList(redisClient *c, robj *o) {
	robj *tail;

	// 链表中无缓存块,直接将对象追加到链表中
	if (listLength(c->reply) == 0) {
		incrRefCount(o); // 增加引用计数
		listAddNodeTail(c->reply, o);
		c->reply_bytes += getStringObjectSdsUsedMemory(o);
	}
	else {
		tail = listNodeValue(listLast(c->reply)); // 取出表尾的sds
		// 如果表尾sds的已用空间加上对象的长度,小于REDIS_REPLY_CHUNK_BYTES
		// 那么将新对象的内容拼接到sds的末尾.
		if (tail->ptr != NULL &&
			tail->encoding == REDIS_ENCODING_RAW &&
			sdslen(tail->ptr) + sdslen(o->ptr) <= REDIS_REPLY_CHUNK_BYTES) {
			c->reply_bytes -= zmalloc_size_sds(tail->ptr);
			tail = dupLastObjectIfNeeded(c->reply);
			// 将内容拼接起来
			tail->ptr = sdscatlen(tail->ptr, o->ptr, sdslen(o->ptr));
			c->reply_bytes += zmalloc_size_sds(tail->ptr);
		}
		else { /* 直接将对象追加到末尾 */
			incrRefCount(o);
			listAddNodeTail(c->reply, o);
			c->reply_bytes += getStringObjectSdsUsedMemory(o);
		}
	}
	/* 检查回复缓冲区的大小，如果超过系统限制的话，那么关闭客户端
	* 不过在这个版本的代码里,起不到作用.
	*/
	asyncCloseClientOnOutputBufferLimitReached(c);
}


/*
 * 负责传送命令回复的写处理器
 */
void sendReplyToServer(aeEventLoop *el, int fd, void *privdata, int mask) {
	//printf("sendReplyToServer\n");
	redisClient *c = privdata;
	int nwritten = 0, totwritten = 0;
	int objlen;
	size_t objmem;
	robj *o;

		aeDeleteFileEvent(server.el, fd, AE_WRITABLE);
	aeCreateFileEvent(server.el, fd, AE_READABLE, readQueryFromServer, c);


		//	printf("sendReplyToServer ccc %s\n", c->buf);
	// 一直循环,直到回复缓冲区为空
	//|| listLength(c->reply)
	//printf(" sendReplyToServer end %d\n", c->bufpos);

	while (c->bufpos > 0 ) 
	{
		if (c->bufpos > 0) 
		{
			nwritten = write(fd, c->buf + c->sentlen, c->bufpos - c->sentlen);
			if (nwritten <= 0) break; // 有可能是真的出错,也有可能是系统的缓冲区已满
			// 成功写入则更新写入计数器变量 
			c->sentlen += nwritten;
			totwritten += nwritten;
			// 如果缓冲区的内容已经全部写入完毕,那么清空客户端的两个计数器变量
			if (c->sentlen == c->bufpos) 
			{
				c->bufpos = 0;
				c->sentlen = 0;
			}
		}
		else {
			// 取出位于链表最前面的对象
			o = listNodeValue(listFirst(c->reply));
			objlen = sdslen(o->ptr);
			nwritten = write(fd, ((char *)o->ptr) + c->sentlen, objlen - c->sentlen);
			if (nwritten <= 0) break;
			c->sentlen += nwritten;
			totwritten += nwritten;

			// 如果缓冲区的内容全部写入完毕,那么删除已经写入完毕的节点
			if (c->sentlen == objlen) {
				listDelNode(c->reply, listFirst(c->reply));
				c->sentlen = 0;
			}
		}
	}

}

/*
 * 负责传送命令回复的写处理器
 */
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
	redisClient *c = privdata;
	int nwritten = 0, totwritten = 0;
	int objlen;
	size_t objmem;
	robj *o;

	// 一直循环,直到回复缓冲区为空
	while (c->bufpos > 0 || listLength(c->reply)) 
	{
		if (c->bufpos > 0) 
		{
			nwritten = write(fd, c->buf + c->sentlen, c->bufpos - c->sentlen);
			if (nwritten <= 0) break; // 有可能是真的出错,也有可能是系统的缓冲区已满
			// 成功写入则更新写入计数器变量 
			c->sentlen += nwritten;
			totwritten += nwritten;
			// 如果缓冲区的内容已经全部写入完毕,那么清空客户端的两个计数器变量
			if (c->sentlen == c->bufpos) 
			{
				c->bufpos = 0;
				c->sentlen = 0;
			}
		}
		else {
			// 取出位于链表最前面的对象
			o = listNodeValue(listFirst(c->reply));
			objlen = sdslen(o->ptr);
			objmem = getStringObjectSdsUsedMemory(o);

			if (objlen == 0) { /* 略过空对象 */
				listDelNode(c->reply, listFirst(c->reply));
				c->reply_bytes -= objmem;
				continue;
			}

			nwritten = write(fd, ((char *)o->ptr) + c->sentlen, objlen - c->sentlen);
			if (nwritten <= 0) break;
			c->sentlen += nwritten;
			totwritten += nwritten;

			// 如果缓冲区的内容全部写入完毕,那么删除已经写入完毕的节点
			if (c->sentlen == objlen) {
				listDelNode(c->reply, listFirst(c->reply));
				c->sentlen = 0;
				c->reply_bytes -= objmem;
			}
		}

	}
	/* 写入出错检查 */
	if (nwritten == -1) {
		if (errno == EAGAIN) {
			nwritten = 0;
		}
		else {
		//	mylog("Error writing to client: %s", strerror(errno));
			freeClient(c);
			return;
		}
	}

	//if (c->bufpos == 0 && listLength(c->reply) == 0)
	{
		c->sentlen = 0;
		//mylog("%s", "close write  handler");
		/* 删除 write handler */
		aeDeleteFileEvent(server.el, c->fd, AE_WRITABLE); /* 这里很重要,如果不关闭的话,下次还会通知,尽管你没有什么数据要发送 */
		
	aeCreateFileEvent(server.el, c->fd, AE_READABLE, readQueryFromClient, c);
		/* 如果指定了写入之后关闭客户端 FLAG ，那么关闭客户端 */
		if (c->flags & REDIS_CLOSE_AFTER_REPLY) freeClient(c);
	}
}

int prepareClientToWrite(redisClient *c) {
//	if (c->fd <= 0) return REDIS_ERR; /* 伪客户端总是不可以写的 */
	/* 一般情况下,为客户端套接字安装写处理器到事件循环 */
	if (c->bufpos == 0 && listLength(c->reply) == 0 &&
		aeCreateFileEvent(server.el, c->fd, AE_WRITABLE, sendReplyToClient, c) == AE_ERR)
		return REDIS_ERR;
	return REDIS_OK;
}

/*
 * 创建一个新的客户端
 */
redisClient *createServer(int fd) {
	redisClient *c = zmalloc(sizeof(redisClient));
	// 当 fd 不为 -1 时，创建带网络连接的客户端
	// 如果 fd 为 -1 ，那么创建无网络连接的伪客户端
	// 因为 Redis 的命令必须在客户端的上下文中使用，所以在执行 Lua 环境中的命令时
	// 需要用到这种伪终端. 
	if (fd != -1) {
		anetNonBlock(NULL, fd);
		anetEnableTcpNoDelay(NULL, fd); // 禁用 Nagle算法

		if (server.tcpkeepalive) {
			anetKeepAlive(NULL, fd, server.tcpkeepalive);
		}

		// 绑定读事件到事件loop(开始接收命令请求)
//		if (aeCreateFileEvent(server.el, fd, AE_READABLE,
//			readQueryFromServer, c) == AE_ERR) {
//			close(fd);
//			zfree(c);
//			return NULL;
//		}
	}

	// 使用默认数据库
	//selectDb(c, 0);

	/* 初始化各个属性 */
	c->fd = fd;
	c->name = NULL;
	c->bufpos = 0; // 回复缓冲区的偏移量
	c->querybuf = sdsempty();
	c->reqtype = 0; // 命令请求的类型
	c->argc = 0; // 命令参数的数量
	c->argv = NULL; // 命令参数
	c->cmd = c->lastcmd = NULL; // 当前执行的命令和最近一次执行的命令
	c->lastinteraction = server.unixtime; // 最后一次互动时间
	c->bulklen = -1; // 读入的参数的长度
	c->multibulklen = 0; // 查询缓冲区中未读入的命令内容数量

	c->reply = listCreate(); // 回复链表 
	c->reply_bytes = 0; //  回复链表的字节量
	
	listSetFreeMethod(c->reply, decrRefCountVoid);
	listSetDupMethod(c->reply, dupClientReplyValue);

	if (fd != -1) listAddNodeTail(server.clients, c); // 如果不是伪客户端,那么添加服务器的客户端到客户端链表之中
	
	return c;

}

/*
 * 创建一个新的客户端
 */
redisClient *createClient(int fd) {
	redisClient *c = zmalloc(sizeof(redisClient));
	// 当 fd 不为 -1 时，创建带网络连接的客户端
	// 如果 fd 为 -1 ，那么创建无网络连接的伪客户端
	// 因为 Redis 的命令必须在客户端的上下文中使用，所以在执行 Lua 环境中的命令时
	// 需要用到这种伪终端. 
	if (fd != -1) {
		anetNonBlock(NULL, fd);
		anetEnableTcpNoDelay(NULL, fd); // 禁用 Nagle算法

		if (server.tcpkeepalive) {
			//anetKeepAlive(NULL, fd, server.tcpkeepalive);
		}

		// 绑定读事件到事件loop(开始接收命令请求)
		if (aeCreateFileEvent(server.el, fd, AE_READABLE,
			readQueryFromClient, c) == AE_ERR) {
			close(fd);
			zfree(c);
			return NULL;
		}
	}

	// 使用默认数据库
	//selectDb(c, 0);

	/* 初始化各个属性 */
	c->fd = fd;
	c->name = NULL;
	c->bufpos = 0; // 回复缓冲区的偏移量
	c->sentlen = 0;
	c->querybuf = sdsempty();
	c->reqtype = 0; // 命令请求的类型
	c->argc = 0; // 命令参数的数量
	c->argv = NULL; // 命令参数
	c->cmd = c->lastcmd = NULL; // 当前执行的命令和最近一次执行的命令
	c->lastinteraction = server.unixtime; // 最后一次互动时间
	c->bulklen = -1; // 读入的参数的长度
	c->multibulklen = 0; // 查询缓冲区中未读入的命令内容数量

	c->reply = listCreate(); // 回复链表 
	c->reply_bytes = 0; //  回复链表的字节量
	c->flags = 0; /* 设置参数 */

	listSetFreeMethod(c->reply, decrRefCountVoid);
	listSetDupMethod(c->reply, dupClientReplyValue);

	if (fd != -1) listAddNodeTail(server.clients, c); // 如果不是伪客户端,那么添加服务器的客户端到客户端链表之中
	c->watched_keys = listCreate(); /* 进行事务时监视的键 */
	initClientMultiState(c); /* 初始化客户端的事务状态 */
	return c;

}

int prepareToWriteServer(redisClient *c) {
	// 一般情况下,为客户端套接字安装写处理器到事件循环
	/*
	if (c->bufpos == 0 && listLength(c->reply) == 0 &&
		aeCreateFileEvent(server.el, c->fd, AE_WRITABLE, sendReplyToClient, c) == AE_ERR)
		return REDIS_ERR;
		*/
	//	printf("prepareToWriteServer %d\n", c->fd);	
	if (c->bufpos == 0  && aeCreateFileEvent(server.el, c->fd, AE_WRITABLE, sendReplyToServer, c) == AE_ERR)
		return REDIS_ERR;
	return REDIS_OK;
}

//void addReply2(redisClient *c, char *msg) {
void addReply2(redisClient *c, robj *obj) {
	//printf("addReplyBulkLen2 %s\n", (char*)(obj->ptr));
	// 为客户端安装写处理器到事件循环
	if (prepareToWriteServer(c) != REDIS_OK) {
		//printf("prepareToWriteServer failure\n");
		return;
		
	}

	// 如果对象的编码为RAW,并且静态缓冲区有空间,那么就可以在不弄乱内存页的情况下,将对象发送给客户端
//	if (sdsEncodedObject(obj)) 
		{
		// 首先尝试复制内容得到 c->buf 中,这样可以避免内存分配
		//char *str = obj->ptr;
		if (_addReplyToBuffer(c, obj->ptr, sdslen(obj->ptr)) != REDIS_OK) {
		//if (_addReplyToBuffer(c, msg, strlen(msg)) != REDIS_OK) {
			// 如果c->buf中的空间不够,就复制到c->reply链表中
			printf("kkkkkkkkkkkkkkkkkk\n");
			//_addReplyObjectToList(c, obj);
		}else{
			//printf("failue\n");
		}
		//free(obj->ptr);
		//free(obj);
		
	}
//	else if (obj->encoding == REDIS_ENCODING_INT) {
//		if (listLength(c->reply) == 0 && (sizeof(c->buf) - c->bufpos) >= 32) {
//			char buf[32];
//			int len;
//			len = ll2string(buf, sizeof(buf), (long)obj->ptr);
//			if (_addReplyToBuffer(c, buf, len) == REDIS_OK)
//				return;
//		}
//		/*
//		 * 执行到这里的话,代表对象是整数,并且长度大于32位,将其转换为字符串
//		 */
//		obj = getDecodedObject(obj);
//		// 保存到缓存中
//		if(_addReplyToBuffer(c, obj->ptr, sdslen(obj->ptr)) != REDIS_OK)
//			_addReplyObjectToList(c, obj);
//		decrRefCount(obj);
//	}
//	else {
//		// todo
//	}

}

/* -----------------------------------------------------------------------------
* Higher level functions to queue data on the client output buffer.
* The following functions are the ones that commands implementations will call.
* -------------------------------------------------------------------------- */
void addReply(redisClient *c, robj *obj) {
	// 为客户端安装写处理器到事件循环
	if (prepareClientToWrite(c) != REDIS_OK) return;

	// 如果对象的编码为RAW,并且静态缓冲区有空间,那么就可以在不弄乱内存页的情况下,将对象发送给客户端
	if (sdsEncodedObject(obj)) {
		// 首先尝试复制内容得到 c->buf 中,这样可以避免内存分配
		char *str = obj->ptr;
		if (_addReplyToBuffer(c, obj->ptr, sdslen(obj->ptr)) != REDIS_OK) {
			// 如果c->buf中的空间不够,就复制到c->reply链表中
			_addReplyObjectToList(c, obj);
		}
	}
	else if (obj->encoding == REDIS_ENCODING_INT) {
		if (listLength(c->reply) == 0 && (sizeof(c->buf) - c->bufpos) >= 32) {
			char buf[32];
			int len;
			len = ll2string(buf, sizeof(buf), (long)obj->ptr);
			if (_addReplyToBuffer(c, buf, len) == REDIS_OK)
				return;
		}
		/*
		 * 执行到这里的话,代表对象是整数,并且长度大于32位,将其转换为字符串
		 */
		obj = getDecodedObject(obj);
		// 保存到缓存中
		if(_addReplyToBuffer(c, obj->ptr, sdslen(obj->ptr)) != REDIS_OK)
			_addReplyObjectToList(c, obj);
		decrRefCount(obj);
	}
	else {
		// todo
	}

}

/*================================ Parse command ==================================*/

/*
 * 处理内联命令，并创建参数对象
 *
 * 内联命令的各个参数以空格分开，并以 \r\n 结尾
 * 例子：
 *
 * <arg0> <arg1> <arg...> <argN>\r\n
 *
 * 这些内容会被用于创建参数对象，
 * 比如
 *
 * argv[0] = arg0
 * argv[1] = arg1
 * argv[2] = arg2
 */


/*
 * 这个函数相当于parse,处理命令
 */
int processInlineBuffer(redisClient *c) {
	char *newline;
	int argc, j;
	sds *argv, aux;
	size_t querylen;

	newline = strchr(c->querybuf, '\n'); // 寻找一行的结尾

	// 如果接收到了错误的内容,出错
	if (newline == NULL) {
		return REDIS_ERR;
	}

	// 处理\r\n 
	if (newline && newline != c->querybuf && *(newline - 1) == '\r')
		newline--;

	/* 然后根据空格,来分割命令的参数
	 * 比如说 SET msg hello \r\n将被分割成
	 * argv[0] = SET
	 * argv[1] = msg
	 * argv[2] = hello
	 * argc = 3
	 */
	querylen = newline - (c->querybuf);
	aux = sdsnewlen(c->querybuf, querylen);
	argv = sdssplitargs(aux, &argc);
	sdsfree(aux);

	// 从缓冲区中删除已经读取了的内容,剩下的内容是未被读取的
	sdsrange(c->querybuf, querylen + 2, -1);

	if (c->argv) free(c->argv);
	c->argv = zmalloc(sizeof(robj*)*argc);

	// 为每个参数创建一个字符串对象
	for (c->argc = 0, j = 0; j < argc; j++) {
		if (sdslen(argv[j])) {
			// argv[j] 已经是 SDS 了
			// 所以创建的字符串对象直接指向该 SDS
			c->argv[c->argc] = createObject(REDIS_STRING, argv[j]);
			c->argc++;
		}
		else {
			sdsfree(argv[j]);
		}
	}
	zfree(argv);
	return REDIS_OK;
}



/*
 * 将 c->querybuf 中的协议内容转换成 c->argv 中的参数对象
 *
 * 比如 *3\r\n$3\r\nSET\r\n$3\r\nMSG\r\n$5\r\nHELLO\r\n
 * 将被转换为：
 * argv[0] = SET
 * argv[1] = MSG
 * argv[2] = HELLO
 */
int processMultibulkBuffer(redisClient *c) {
	char *newline = NULL;
	int pos = 0, ok;
	long long ll;

	assert(c->argc == 0); // 每一次读取命令的时候都要保证client被reset过
	c->querybuf_server = sdsdup(c->querybuf);
//	printf("processMultibulkBuffer %s\n", c->querybuf_server);
	/* 读入命令的参数个数 */
	// 比如 *3\r\n$3\r\nSET\r\n... 将令 c->multibulklen = 3
	if (c->multibulklen == 0) {
		newline = strchr(c->querybuf, '\r');
		// 将参数个数，也即是 * 之后， \r\n 之前的数字取出并保存到 ll 中 
		// 比如对于 *3\r\n ，那么 ll 将等于 3
		ok = string2ll(c->querybuf + 1, newline - (c->querybuf + 1), &ll);

		// 参数数量之后的位置 
		// 比如对于 *3\r\n$3\r\n$SET\r\n... 来说,
		// pos 指向 *3\r\n$3\r\n$SET\r\n...
		//                ^
		//                |
		//               pos
		pos = (newline - c->querybuf) + 2;

		// 设置参数数量
		c->multibulklen = ll;

		// 根据参数数量,为各个参数对象分配空间 
		if (c->argv) zfree(c->argv);
		c->argv = zmalloc(sizeof(robj*) * c->multibulklen);
	}

	// 从c->querybuf中读入参数,并创建各个参数对象到c->argv
	while (c->multibulklen) {
		// 读入参数长度
		if (c->bulklen == -1) { // 这里指的是命令的长度
			// 确保"\r\n"存在 
			newline = strchr(c->querybuf + pos, '\r');

			// 读取长度,比如说 $3\r\nSET\r\n 会让 ll 的值变成3
			ok = string2ll(c->querybuf + pos + 1, newline - (c->querybuf + pos + 1), &ll);

			// 定位到参数的开头
			// 比如 
			// $3\r\nSET\r\n...
			//       ^
			//       |
			//      pos
			pos += newline - (c->querybuf + pos) + 2;
			c->bulklen = ll;
		}
		
		// 为参数创建字符串对象
		if (pos == 0 &&
			c->bulklen >= REDIS_MBULK_BIG_ARG &&
			(signed)sdslen(c->querybuf) == c->bulklen + 2)
		{
			c->argv[c->argc++] = createObject(REDIS_STRING, c->querybuf);
			sdsIncrLen(c->querybuf, -2); // 去掉\r\n
			c->querybuf = sdsempty();
			c->querybuf = sdsMakeRoomFor(c->querybuf, c->bulklen + 2);
			pos = 0;
		}
		else {
			c->argv[c->argc++] =
				createStringObject(c->querybuf + pos, c->bulklen);
			pos += c->bulklen + 2;
		}

		// 清空参数长度
		c->bulklen = -1;

		// 减少还需读入的参数个数
		c->multibulklen--;
	}
	
	if (pos) sdsrange(c->querybuf, pos, -1); // 从querybuf中删除已被读取的内容

	// 如果本条命令的所有参数都已经读取完,那么返回
	if (c->multibulklen == 0) return REDIS_OK;

	// 如果还有参数未读取完,那么就是协议出错了!
	return REDIS_ERR;
}

/*
 * 清空所有命令参数
 */
void freeClientArgv(redisClient *c) {
	int j;
	for (j = 0; j < c->argc; j++)
		decrRefCount(c->argv[j]);
	c->argc = 0;
	c->cmd = NULL;
}

/* 
 * 在客户端执行完命令之后执行：重置客户端以准备执行下个命令 
 */
void resetClient(redisClient *c) {
	redisCommandProc *prevcmd = c->cmd ? c->cmd->proc : NULL;
	freeClientArgv(c);
	c->reqtype = 0;
	c->multibulklen = 0;
	c->bulklen = -1;
}

void processInputBuffer(redisClient *c) {
	// 尽可能地处理查询缓存区中的内容.如果读取出现short read, 那么可能会有内容滞留在读取缓冲区里面
	// 这些滞留的内容也许不能完整构成一个符合协议的命令,需要等待下次读事件的就绪.
	while (sdslen(c->querybuf)) {

		if (!c->reqtype) {
			if (c->querybuf[0] == '*') {
				c->reqtype = REDIS_REQ_MULTIBULK; // 多条查询 
			}
			else {
				c->reqtype = REDIS_REQ_INLINE; // 内联查询 
			}
		}
		// 将缓冲区的内容转换成命令,以及命令参数
		if (c->reqtype == REDIS_REQ_INLINE) {
			if (processInlineBuffer(c) != REDIS_OK) break;
		}
		else if (c->reqtype == REDIS_REQ_MULTIBULK) {
			if (processMultibulkBuffer(c) != REDIS_OK) break;
		}
		else {
			// todo
		}

		if (c->argc == 0) {
			resetClient(c); // 重置客户端
		}
		else {
			if (processCommand(c) == REDIS_OK)
				resetClient(c);
		}
	}
}

int redis_reply(char* msg) {
	char* ptr;
	char* cur;
	int argc = -2;
	int cnt;
	//	cur = &msg[*read_pos];
	//	cur = *(msg+(*read_pos));
	//	printf("uuuuuu %s\n", cur);
	cur= msg;

	int read_pos=0;
	while( (ptr = strstr(cur, "\r\n") ) !=NULL) {
			
		read_pos += ptr - cur + 2;
		char* tmp = NULL;

		if (argc <  -1){
			cnt = 0;	
			switch(*cur){
				case '*':
					cur++;
					argc =  strtol(cur, &tmp, 10) *2;	
					break;
				case '$':
					argc = 1;
					break;
				case '+':
					return read_pos;
			}
			
		} else {
			if (++cnt == argc) {
				break;
			}
		}
		cur = &msg[read_pos];
	}
	return read_pos;
}

/*
 * 读取客户端的查询缓冲区内容
 */
void readQueryFromServer(aeEventLoop *el, int fd, void *privdata, int mask) {
	redisClient *c = (redisClient *)privdata;
	int nread, readlen;
	size_t qblen;

	server.current_client = c; // 设置服务器的当前客户端

	readlen = REDIS_IOBUF_LEN; // 读入长度,默认为16MB

	// 获取查询缓冲区当前内容的长度 
	qblen = sdslen(c->querybuf); 

	// 为查询缓冲区分配空间
	c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);
	// 读入内容到查询缓存
	nread = read(fd, c->querybuf + qblen, readlen);
	//printf("readQueryFromServer %s\n", c->querybuf);
	if (nread == -1) {
		if (errno == EAGAIN) {
			nread = 0;
		} 
		else {
			//mylog("Reading from client: %s", strerror(errno));
			return;
		}
	}
	else if (nread == 0) { // 对方关闭了连接
	//	mylog("%s", "Client closed connection");
		freeClient(c);
		return;
	}

	if (nread) {
		sdsIncrLen(c->querybuf, nread);
		c->lastinteraction = server.unixtime; // 更新最后一次互动的时间
	} 
	else { 
		// 在 nread == -1 且 errno == EAGAIN 时运行
		server.current_client = NULL;
		return;
	}
	//
	// 从查询缓存中读取内容,创建参数,并执行命令,函数会执行到缓存中的所有内容都被处理完为止
	//

	//这里不要执行了
	//processInputBuffer(c);
	server.current_client = NULL;

	/*
	当cmd为set 返回shared.ok,当get时，不能返回这个
	zhangming
	*/
//	if(c->client->argv[0] == "SET") {
//		
//	} 
//	else if (c->client->argv[0] == "GET") {
//	
//	if(strstr(c->querybuf, "SET")){
//		addReply(c->client,  shared.ok);
//		
//	}else{
//		robj *o = createRawStringObject(c->querybuf, sdslen(c->querybuf));
// 		addReply(c->client, o);
//		//fuck 这里一样清空，不然会累积的,这个是server的属性
//		
//	}

	printf("c->querybuf %s\n", c->querybuf);
	int num = c->queue.size;
	printf("c->queue.size %d\n", c->queue.size);
	int read_pos2=0;
	int len = strlen(c->querybuf);
	int total=0;
	char* res= NULL;

//	while(num >0){
	while(total < len) {
		redisClient *cc = (redisClient *)rp_queue_shift(&c->queue);
//		printf("client fd is %d\n", cc->fd);
		num--;

		read_pos2 = redis_reply(c->querybuf+total);

		res = (char*)malloc(sizeof(char)* read_pos2+1);
		memset(res, 0, read_pos2+1);
		memcpy(res, c->querybuf+total, read_pos2);
		printf("result is %s\n", res);
		free(res);

		total+= read_pos2;
//		addReply(cc,  shared.ok);
		printf("%s\n", c->querybuf);
	//		if(strstr(c->querybuf, "+")){
	
		//set命令,返回ok
		if(c->type == RETURN_SET || c->type== RETURN_HMSET){
			addReply(cc,  shared.ok);
		}else{
			robj *o = createRawStringObject(c->querybuf, sdslen(c->querybuf));
			addReply(cc, o);
			//fuck 这里一样清空，不然会累积的,这个是server的属性
		} 
	}
//
///redisClient *cc = rp_queue_shift(&c->queue)
printf("server 1379 fd is %d\n", c->fd);
//addReply(c->client,  shared.ok);
sdsclear(c->querybuf);

	//aeDeleteFileEvent(server.el, c->fd, AE_READABLE);
}

/*
 * 读取客户端的查询缓冲区内容
 */
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {

	redisClient *c = (redisClient *)privdata;
//		printf("readQueryFromClient fd is %d\n", c->fd);
	int nread, readlen;
	size_t qblen;

	server.current_client = c; // 设置服务器的当前客户端

	readlen = REDIS_IOBUF_LEN; // 读入长度,默认为16MB

	// 获取查询缓冲区当前内容的长度 
	qblen = sdslen(c->querybuf); 

	// 为查询缓冲区分配空间
	c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);
	// 读入内容到查询缓存
	nread = read(fd, c->querybuf + qblen, readlen);

	if (nread == -1) {
		if (errno == EAGAIN) {
			nread = 0;
		} 
		else {
		//	mylog("Reading from client: %s", strerror(errno));
			return;
		}
	}
	else if (nread == 0) { // 对方关闭了连接
	//mylog("%s", "Client closed connection");
		freeClient(c);
		return;
	}

	if (nread) {
		sdsIncrLen(c->querybuf, nread);
		c->lastinteraction = server.unixtime; // 更新最后一次互动的时间
	} 
	else {
		// 在 nread == -1 且 errno == EAGAIN 时运行
		server.current_client = NULL;
		return;
	}
	//
	// 从查询缓存中读取内容,创建参数,并执行命令,函数会执行到缓存中的所有内容都被处理完为止
	//
	processInputBuffer(c);
	server.current_client = NULL;
}


/*
 * TCP 连接 accept 处理器
 */
#define MAX_ACCEPTS_PER_CALL 1000
static void acceptCommonHandler(int fd, int flags) {
	// 创建客户端
	redisClient *c;
	// 在createClient函数中大概做了这么一些事情,首先是构造了一个redisClient结构,
	// 最为重要的是,完成了fd与RedisClient结构的关联,并且完成了该结构的初始化工作.
	// 并将这个结果挂到了server的clients链表上.
	// 还有一点,那就是对该fd的读事件进行了监听.
	if ((c = createClient(fd)) == NULL) {
		// log
		close(fd);
		return;
	}
}


/*
 * 创建一个 TCP 连接处理器
 */
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
	int cport, cfd;
	char cip[REDIS_IP_STR_LEN];

	for (; ; ) {
		cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport); // 获得ip地址以及端口号
		//printf("111111111111111111111 client fd is %d %d\n", cfd, cport);
		if (cfd == ANET_ERR) {
			break; // 出错直接终止循环 
			if (errno != EWOULDBLOCK) {
				// log
				return;
			}
		}
		acceptCommonHandler(cfd, 0); // 为客户端创建客户端状态
	}
}


void addReplyErrorLength(redisClient *c, char *s, size_t len) {
	addReplyString(c, "-ERR ", 5);
	addReplyString(c, s, len);
	addReplyString(c, "\r\n", 2);
}

/*
 * 返回一个错误回复
 *
 * 例子 -ERR unknown command 'foobar'
 */
void addReplyError(redisClient *c, char *err) {
	addReplyErrorLength(c, err, strlen(err));
}

/* 
 * 返回一个整数回复
 *
 * 格式为: 10086\r\n
 */
void addReplyLongLong(redisClient *c, long long ll) {
	if (ll == 0)
		addReply(c, shared.czero);
	else if (ll == 1)
		addReply(c, shared.cone);
	else
		addReplyLongLongWithPrefix(c, ll, ':');
}

/*
* 返回一个c缓冲区作为回复
*/
void addReplyBulkBuffer(redisClient *c, void *p, size_t len) {
	addReplyLongLongWithPrefix(c, len, '$');
	addReplyString(c, p, len);
	addReply(c, shared.crlf);
}




void addReplyMultiBulkLen(redisClient *c, long length) {
	if (length < REDIS_SHARED_BULKHDR_LEN)
		addReply(c, shared.mbulkhdr[length]);
	else
		addReplyLongLongWithPrefix(c, length, '*');
}

/*
 * 返回一个c缓冲区作为回复
 */
void addReplyBulkCBuffer(redisClient*c, void *p, size_t len) {
	addReplyLongLongWithPrefix(c, len, '$');
	addReplyString(c, p, len);
	addReply(c, shared.crlf);
}

/*
 * 返回一个long long值作为回复
 */
void addReplyBulkLongLong(redisClient *c, long long ll) {
	char buf[64];
	int len;

	len = ll2string(buf, 64, ll);
	addReplyBulkCBuffer(c, buf, len);
}

/*
 * 当发送MultiBulk回复的时候,先创建一个空的链表,之后再用实际的回复填充它
 */
void *addDeferredMultiBulkLength(redisClient *c) {
	if (prepareClientToWrite(c) != REDIS_OK) return NULL;
	listAddNodeTail(c->reply, createObject(REDIS_STRING, NULL));
	return listLast(c->reply);
}


/* 设置Multi BULK回复的长度 */
void setDeferredMultiBulkLength(redisClient *c, void *node, long length) {
	listNode *ln = (listNode *)node;
	robj *len, *next;
	if (node == NULL) return;

	len = listNodeValue(ln);
	len->ptr = sdscatprintf(sdsempty(), "*%ld\r\n", length);
	char *str1 = len->ptr;
	len->encoding = REDIS_ENCODING_RAW;
	c->reply_bytes += zmalloc_size_sds(len->ptr);
	if (ln->next != NULL) {
		next = listNodeValue(ln->next);
		if (next->ptr != NULL) {
			c->reply_bytes -= zmalloc_size_sds(len->ptr);
			c->reply_bytes -= getStringObjectSdsUsedMemory(next);
			len->ptr = sdscatlen(len->ptr, next->ptr, sdslen(next->ptr));
			c->reply_bytes += zmalloc_size_sds(len->ptr);
			listDelNode(c->reply, ln->next);
		}
	}
	asyncCloseClientOnOutputBufferLimitReached(c);
}

/*
 * 返回一个 C 字符串作为回复
 */
void addReplyBulkCString(redisClient *c, char *s) {
	if (s == NULL) {
		addReply(c, shared.nullbulk);
	}
	else {
		addReplyBulkCBuffer(c, s, strlen(s));
	}
}

/*
 * 以 bulk 回复的形式，返回一个双精度浮点数
 *
 * 例子 $4\r\n3.14\r\n
 */
void addReplyDouble(redisClient *c, double d) {
	char dbuf[128], sbuf[128];
	int dlen, slen;
	if (isinf(d)) {
		/* Libc in odd systems (Hi Solaris!) will format infinite in a
		* different way, so better to handle it in an explicit way. */
		addReplyBulkCString(c, d > 0 ? "inf" : "-inf");
	}
	else {
		dlen = snprintf(dbuf, sizeof(dbuf), "%.17g", d);
		slen = snprintf(sbuf, sizeof(sbuf), "$%d\r\n%s\r\n", dlen, dbuf);
		addReplyString(c, sbuf, slen);
	}
}

/* 异步地释放给定的客户端
 * 所谓异步,指的是不是立刻释放,而是稍后再释放
 */
void freeClientAsync(redisClient *c) {
	if (c->flags & REDIS_CLOSE_ASAP) return;
	c->flags |= REDIS_CLOSE_ASAP;
	listAddNodeTail(server.clients_to_close, c);
}

/* 关闭需要异步关闭的客户端 */
void freeClientsInAsyncFreeQueue(void) {
	/* 遍历所有要关闭的客户端 */
	while (listLength(server.clients_to_close)) {
		listNode *ln = listFirst(server.clients_to_close);
		redisClient *c = listNodeValue(ln);
		c->flags &= ~REDIS_CLOSE_ASAP;
		/* 关闭客户端 */
		freeClient(c);
		/* 从客户端链表中删除被关闭的客户端 */
		listDelNode(server.clients_to_close, ln);
	}
}

/* 
 * 这个函数检查客户端是否达到了输出缓冲区的软性（soft）限制或者硬性（hard）限制，
 * 并在到达软限制时，对客户端进行标记。
 *
 * 返回值：到达软性限制或者硬性限制时，返回非 0 值。
 *         否则返回 0 。
 */
int checkClientOutputBufferLimits(redisClient *c) {
	// todo
	return 0;
}

/* 
 * 如果客户端达到缓冲区大小的软性或者硬性限制，那么打开客户端的 ``REDIS_CLOSE_ASAP`` 状态，
 * 让服务器异步地关闭客户端。
 *
 * 注意：
 * 我们不能直接关闭客户端，而要异步关闭的原因是客户端正处于一个不能被安全地关闭的上下文中。
 * 比如说，可能有底层函数正在推入数据到客户端的输出缓冲区里面。
 */
void asyncCloseClientOnOutputBufferLimitReached(redisClient *c) {
	assert(c->reply_bytes < ULONG_MAX - (1024 * 64));

	/* 已经被标记了 */
	if (c->reply_bytes == 0 || c->flags & REDIS_CLOSE_ASAP) return;

	/* 检查限制 */
	if (checkClientOutputBufferLimits(c)) {
		/* 异步关闭 */
		freeClientAsync(c);
	}
}

/* 让服务器在被阻塞的情况下，仍然处理某些事件。 */
int processEventsWhileBlocked(void) {
	int iterations = 4; /* See the function top-comment. */
	int count = 0;
	while (iterations--) {
		int events = aeProcessEvents(server.el, AE_FILE_EVENTS | AE_DONT_WAIT);
		if (!events) break;
		count += events;
	}
	return count;
}

void addReplyErrorFormat(redisClient *c, const char *fmt, ...) {
	size_t l, j;
	va_list ap;
	va_start(ap, fmt);
	sds s = sdscatvprintf(sdsempty(), fmt, ap);
	va_end(ap);
	/* Make sure there are no newlines in the string, otherwise invalid protocol
	* is emitted. */
	l = sdslen(s);
	for (j = 0; j < l; j++) {
		if (s[j] == '\r' || s[j] == '\n') s[j] = ' ';
	}
	addReplyErrorLength(c, s, sdslen(s));
	sdsfree(s);
}