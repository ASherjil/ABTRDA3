# AF_XDP

Instructions from: https://docs.ebpf.io/linux/concepts/af_xdp/  

AF_XDP code example: https://github.com/xdp-project/bpf-examples/blob/main/AF_XDP-example/xdpsock.c  

libxdp: https://github.com/xdp-project/xdp-tools/blob/main/lib/libxdp/README.org


## Configuration / Control Plan

1. First step is to create the socket to which we will be attaching the UMEM and ring buffers. 

    We do so by calling the socket syscall as follows:

    `fd = socket(AF_XDP, SOCK_RAW, 0);`

2. Next is to setup and register the UMEM. Then link it to the socket via the `setsockopt` syscall:

    ````
   struct xdp_umem_reg {
    __u64 addr; /* Start of packet data area */
    __u64 len; /* Length of packet data area */
    __u32 chunk_size;
    __u32 headroom;
    __u32 flags;
    };

    struct xdp_umem_reg umem_reg = {
    .addr = (__u64)(void *)umem,
    .len = umem_len,
    .chunk_size = chunk_size,
    .headroom = 0, // see Options, variations, and exceptions
    .flags = 0, // see Options, variations, and exceptions
    };
    if (!setsockopt(fd, SOL_XDP, XDP_UMEM_REG, &umem_reg, sizeof(umem_reg)))
    // handle error
    
3. Next up are our ring buffers. These are allocated by the kernel when we tell the kernel how large we want each ring buffer to be via a `setsockopt` syscall.

    ````
   static const int ring_size = 512; // Set the size of the ring buffers
    if (!setsockopt(fd, SOL_XDP, {XDP_RX_RING,XDP_TX_RING,XDP_UMEM_FILL_RING,XDP_UMEM_COMPLETION_RING}, &ring_size, sizeof(ring_size)))
    // handle error

4. After we have set the sizes for all ring buffers we can request the `mmap` offsets with a `getsockopt` syscall:

    ````
   struct xdp_ring_offset {
    __u64 producer;
    __u64 consumer;
    __u64 desc;
    __u64 flags;
    };
    
    struct xdp_mmap_offsets {
    struct xdp_ring_offset rx;
    struct xdp_ring_offset tx;
    struct xdp_ring_offset fr; /* Fill */
    struct xdp_ring_offset cr; /* Completion */
    };
    
    struct xdp_mmap_offsets offsets = {0};
    
    if (!getsockopt(fd, SOL_XDP, XDP_MMAP_OFFSETS, &offsets, sizeof(offsets)))
    // handle error
   
5. The next step is to map the ring buffers into process memory with the `mmap` syscall:

    ````
   struct xdp_desc {
    __u64 addr;
    __u32 len;
    __u32 options;
    };

    void *{rx,tx,fill,completion}_ring_mmap = mmap(
    NULL,
    offsets.{rx,tx,fr,cr}.desc + ring_size * sizeof(struct xdp_desc),
    PROT_READ|PROT_WRITE,
    MAP_SHARED|MAP_POPULATE,
    fd,
    {XDP_PGOFF_RX_RING,XDP_PGOFF_TX_RING,XDP_UMEM_PGOFF_FILL_RING,XDP_UMEM_PGOFF_COMPLETION_RING});
    if (!{rx,tx,fill,completion}_ring_mmap)
    // handle error
    
    __u32 *{rx,tx,fill,completion}_ring_consumer = {rx,tx,fill,completion}_ring_mmap + offsets.{rx,tx,fr,cr}.consumer;
    __u32 *{rx,tx,fill,completion}_ring_producer = {rx,tx,fill,completion}_ring_mmap + offsets.{rx,tx,fr,cr}.producer;
    struct xdp_desc[ring_size] {rx,tx,fill,completion}_ring = {rx,tx,fill,completion}_ring_mmap + offsets.{rx,tx,fr,cr}.desc;
   
6. We have setup our XSK and we have access to both UMEM and all 4 ring buffers. The last step is to associate our XSK with a network device and queue.

    ````
   struct sockaddr_xdp {
    __u16 sxdp_family;
    __u16 sxdp_flags;
    __u32 sxdp_ifindex;
    __u32 sxdp_queue_id;
    __u32 sxdp_shared_umem_fd;
    };

    struct sockaddr_xdp sockaddr = {
    .sxdp_family = AF_XDP,
    .sxdp_flags = 0, // see Options, variations, and exceptions
    .sxdp_ifindex = some_ifindex, // The actual ifindex is dynamically determined, picking is up to the user.
    .sxdp_queue_id = 0,
    .sxdp_shared_umem_fd = fd, // see Options, variations, and exceptions
    };
    
    if(!bind(fd, &sockaddr, sizeof(struct sockaddr_xdp)))
    // handle error
   

### Mapping Config steps to libxdp functions

1. Steps 1-5 are completed for the `XDP_UMEM_FILL_RING` & `XDP_UMEM_COMPLETION_RING` rings in the `xdpsock.c` as:

   `xsk_configure_umem()` -> `xsk_umem__create()` -> `xsk_umem__create_with_fd()` -> 
   `xsk_umem__create_opts()` : {step 1,2 done here} -> `xsk_create_umeme_rings()` : {steps 3,4,5 done for fill & completion rings}

2. Steps 3,4,5 are completed for `XDP_RX_RING` & `XDP_TX_RING` in the `xdpsock.c` as:

   `xsk_configure_socket()` -> `xsk_socket__create()` -> `xsk_socket__create_shared()` -> `xsk_socket__create_shared()` -> 
   `xsk_socket__create_opts()`: {steps 3,4,5 are here for Tx/Rx rings}   

3. Step 6 where the `bind(...)` is required is also completed at the end of the libxdp function `xsk_socket__create_opts()`.

## Creating a XDP filter 

1. Once a XDP filter program is created it is attached using `libxdp`. `xdpsock.c` examples using the following:

   `load_xdp_program()` -> `xdp_program__attach()` -> `xdp_program__attach_multi()` -> `xdp_program__attach_single()` -> `xdp_program__load()` -> `xdp_program__fill_from_fd()`

   This is quite a complex function inside `libxdp.c` so I provided a simplified flow. 


## Rx/Tx (Data Plane)

This is real deal and likely the most important part. This would reside in the hot path for Rx/Tx. The counter-intuitive 4x SPSC structure is as follows:

1. Producer: Fill & Tx. These use: `xsk_ring_prod` functions.  You have to reserve one or more slots in the producer rings, then when they have filled out, you `submit` them so the kernel can act on them.
2. Consumer: Rx and Completion. These use: `xsk_ring_cons` functions. You `peek` to check if there are any new packets in the ring and if so you can read them. After reading you `release` them back to the kernel so it can be reused.

#### Rx Data Plane code flow

1. [Insert]

#### Tx Data Plan code flow

1. [Insert]