# `ef_vi` C++ coding guide

`ef_vi` is a low-level API to handle and process L2 ethernet frames for Solarflare(AMD) NICs such as Solarflare X2522-PLUS. 
It bypasses the linux kernel and allows busy polling of the Tx/Rx descriptors for low latency. For ultra low latency `ef_vi` has a trick known as CTPIO(cut through programmable IO), which allows the user to Tx small frames directly into the
wire through the NIC without the a) doorbell cadence and b) store and push for the standard programmable IO(PIO). 

This guide is primarily built from canonical sources:

1. PDF coding guide from AMD: https://www.amd.com/content/dam/amd/en/support/downloads/solarflare/onload/openonload/packages/SF-114063-CD-10_ef_vi_User_Guide.pdf
2. `eflatency`. This is the sample application which allows you to measure RTT: https://github.com/majek/openonload/blob/master/src/tests/ef_vi/eflatency.c
3. `rtt_efvi`. Another sample application referenced in the PDF coding guide above(1) : https://github.com/majek/openonload/blob/master/src/tests/rtt/rtt_efvi.c

## Init defaulted to CTPIO 

1. Open the driver, allocate `ef_pd` then alloc `ef_vi` from `ef_pd`:

```c++
ef_driver_handle  driver_handle;
ef_vi		 vi; 
ef_pd        pd;

TRY(ef_driver_open(&driver_handle));
TRY(ef_pd_alloc(&pd, driver_handle, ifindex, pd_flags));

if( cfg_ctpio_no_poison )
vi_flags |= EF_VI_TX_CTPIO_NO_POISON;

/* Try with CTPIO first. */
if( ef_vi_capabilities_get(driver_handle, ifindex, EF_VI_CAP_CTPIO,
                         &capability_val) == 0 && capability_val ) {
vi_flags |= EF_VI_TX_CTPIO;
if( ef_vi_alloc_from_pd(&vi, driver_handle, &pd, driver_handle,
                        -1, -1, -1, NULL, -1, vi_flags) == 0 )
    goto got_vi;
fprintf(stderr, "Failed to allocate VI with CTPIO.\n");
vi_flags &= ~(EF_VI_TX_CTPIO | EF_VI_TX_CTPIO_NO_POISON);
```

2. A filter is specified and added so that the virtual interface receives traffic:

```c++
  ef_filter_spec_init(&filter_spec, EF_FILTER_FLAG_NONE);
  TRY(ef_filter_spec_set_ip4_local(&filter_spec, IPPROTO_UDP, htonl(raddr_he),
                                   htons(port_he)));
  TRY(ef_vi_filter_add(&vi, driver_handle, &filter_spec, NULL));
```

3. Create the packet buffers:

```c++
#define N_RX_BUFS	256u
#define N_TX_BUFS	1u
#define N_BUFS          (N_RX_BUFS + N_TX_BUFS)
#define FIRST_TX_BUF    N_RX_BUFS
#define BUF_SIZE        2048

struct pkt_buf {
  struct pkt_buf* next;
  ef_addr         dma_buf_addr;
  int             id;
  unsigned        dma_buf[1] EF_VI_ALIGN(EF_VI_DMA_ALIGN);
};

struct pkt_buf*          pkt_bufs[N_RX_BUFS + N_TX_BUFS];
static ef_pd             pd;
static ef_memreg         memreg;

  {
    int bytes = N_BUFS * BUF_SIZE;
    void* p;
    TEST(posix_memalign(&p, 4096, bytes) == 0);
    TRY(ef_memreg_alloc(&memreg, driver_handle, &pd, driver_handle, p, bytes));
    for( i = 0; i <= N_RX_BUFS; ++i ) {
      pkt_bufs[i] = (void*) ((char*) p + i * BUF_SIZE);
      pkt_bufs[i]->dma_buf_addr = ef_memreg_dma_addr(&memreg, i * BUF_SIZE);
    }
  }

  for( i = 0; i <= N_RX_BUFS; ++i ) {
    pb = pkt_bufs[i];
    pb->id = i;
    pb->dma_buf_addr += offsetof(struct pkt_buf, dma_buf);
  }
```

4. 