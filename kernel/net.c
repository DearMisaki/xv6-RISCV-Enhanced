#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static struct spinlock netlock;

#define NSOCK 32
#define SOCK_MAXQUEUE 16

struct sock {
  struct spinlock lock;
  short port;
  char *queue[SOCK_MAXQUEUE];

  int head;
  int tail;
};

static struct sock sockets[NSOCK];

void
netinit(void)
{
  initlock(&netlock, "netlock");

  for (int i = 0; i < NSOCK; ++i)
  {
    sockets[i].head = 0;
    sockets[i].tail = 0;
    sockets[i].port = 0;
    initlock(&sockets[i].lock, "socket");
  }

}


//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  int port;
  argint(0, &port);

  for (int i = 0; i < NSOCK; ++i)
  {
    acquire(&sockets[i].lock);
    if (sockets[i].port == 0)
    {
      sockets[i].port = port;
      sockets[i].head = 0;
      sockets[i].tail = 0;
      release(&sockets[i].lock);
      return 0;
    }
    release(&sockets[i].lock);
  }

  return -1;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  int port;
  argint(0, &port);

  for(int i = 0; i < NSOCK; i++) {
    acquire(&sockets[i].lock);
    if(sockets[i].port == port) {
      
      // 释放队列中还没有被读取的包，防止内存泄漏
      while(sockets[i].head != sockets[i].tail) {
        kfree(sockets[i].queue[sockets[i].head]);
        sockets[i].head = (sockets[i].head + 1) % SOCK_MAXQUEUE;
      }
      sockets[i].port = 0;
      release(&sockets[i].lock);
      return 0;
    }
    release(&sockets[i].lock);
  }
  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
uint64
sys_recv(void)
{
  
  int dport;
  uint64 src_addr;
  uint64 sport_addr;
  uint64 buf_addr;
  int maxlen;

  argint(0, &dport);
  argaddr(1, &src_addr);
  argaddr(2, &sport_addr);
  argaddr(3, &buf_addr);
  argint(4, &maxlen);

struct sock *sk = 0;
  for(int i = 0; i < NSOCK; i++) {
    acquire(&sockets[i].lock);
    if(sockets[i].port == dport) {
      sk = &sockets[i];
      break;
    }
    release(&sockets[i].lock);
  }

  if(!sk) 
    return -1; // 尝试从一个未绑定的端口接收

  // 如果队列为空，进程在此休眠等待
  while(sk->head == sk->tail) {
    if(myproc()->killed) {
      release(&sk->lock);
      return -1;
    }
    sleep(&sk->head, &sk->lock);
  }

  // 取出队列中的第一个包
  char *pkt = sk->queue[sk->head];
  sk->head = (sk->head + 1) % SOCK_MAXQUEUE;
  release(&sk->lock);

  // 解析各个头部
  struct eth *eth = (struct eth *)pkt;
  struct ip *ip = (struct ip *)(eth + 1);
  struct udp *udp = (struct udp *)(ip + 1);
  char *payload = (char *)(udp + 1);

  // 获取源 IP 和 源端口（必须使用大端转小端宏进行转换）
  uint32 src = ntohl(ip->ip_src);
  uint16 sport = ntohs(udp->sport);
  
  // 计算实际的数据载荷长度
  int payload_len = ntohs(udp->ulen) - sizeof(struct udp);
  int copylen = (maxlen < payload_len) ? maxlen : payload_len;

  struct proc *p = myproc();

  if(copyout(p->pagetable, src_addr, (char *)&src, sizeof(src)) < 0 ||
     copyout(p->pagetable, sport_addr, (char *)&sport, sizeof(sport)) < 0 ||
     copyout(p->pagetable, buf_addr, payload, copylen) < 0) {
    kfree(pkt);
    return -1;
  }

  kfree(pkt);
  return copylen;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  e1000_transmit(buf, total);

  return 0;
}

void udp_recv(char *buf, int len)
{
  struct eth *ineth = (struct eth*)buf;
  struct ip *inip = (struct ip *)(ineth + 1);

  struct udp *inudp = (struct udp *)(inip + 1);

  int dport = ntohs(inudp->dport);

  for (int i = 0; i < NSOCK; ++i)
  {
    acquire(&sockets[i].lock);
    if (sockets[i].port == dport)
    {
      int next = (sockets[i].tail + 1) % SOCK_MAXQUEUE;

      if (next == sockets[i].head)
      {
        release(&sockets[i].lock);
        kfree(buf);
        return;
      }

      // 缓冲区有空闲空间
      sockets[i].queue[sockets[i].tail] = buf;

      sockets[i].tail = next;

      wakeup(&sockets[i].head);

      release(&sockets[i].lock);

      return;
    }

    release(&sockets[i].lock);
  }

  // 无匹配端口
  kfree(buf);
}

void
ip_rx(char *buf, int len)
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  struct eth *ineth = (struct eth*)buf;
  struct ip *inip = (struct ip *)(ineth + 1);

  if (inip->ip_p == IPPROTO_UDP)
  {
    udp_recv(buf, len);
  }
  else
  {
    kfree(buf);
    return;
  }

}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
