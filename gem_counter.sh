#!/bin/bash
# Dump Cadence GEM hardware counters — reads via /dev/mem.
# Works whether the PMD is running or macb is bound: both map the same
# physical registers, and this script only READS, never writes.
set -u
GEM=0xff0b0000

print_reg() {
    local name=$1 off=$2
    local addr
    addr=$(printf "0x%x" $(( GEM + off )))
    # Capture stdout AND stderr — some devmem2 variants print to stderr.
    local raw
    raw=$(sudo devmem2 "$addr" w 2>&1)
    # Extract the LAST hex value in devmem2's output, regardless of its
    # exact wording ("Read at ...", "Value at ...", etc.).
    local val
    val=$(grep -oE '0x[0-9A-Fa-f]+' <<<"$raw" | tail -1)
    if [[ -n "${val:-}" ]]; then
        printf "  %-16s @ %s = %s\n" "$name" "$addr" "$val"
    else
        printf "  %-16s @ %s = <no value — raw output below>\n" "$name" "$addr"
        echo "$raw" | sed 's/^/      /'
    fi
}

echo "─── GEM counters @ $GEM ───"
print_reg OCTTXL      0x100
print_reg TXCNT       0x108
print_reg TXBCCNT     0x10c
print_reg OCTRXL      0x150
print_reg RXCNT       0x158
print_reg RXBROADCNT  0x15c
print_reg RXMULTICNT  0x160
print_reg RXPAUSECNT  0x164
print_reg RXJABCNT    0x18c
print_reg RXFCSCNT    0x190
print_reg RXALIGNCNT  0x19c
print_reg RXRESERRCNT 0x1a0
print_reg RXORCNT     0x1a4
echo "─── NCR / TSR / RSR / DMACFG ───"
print_reg NCR         0x000
print_reg TSR         0x014
print_reg RSR         0x020
print_reg DMACFG      0x010
echo "─── Per-queue descriptor base pointers ───"
print_reg TBQP_Q0     0x440
print_reg TBQP_Q1     0x444
print_reg RBQP_Q0     0x480
print_reg RBQP_Q1     0x484                     