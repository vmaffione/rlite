#!/bin/bash

source tests/prologue.sh
source tests/env.sh

IF=eth0
if [ -n "$1" ]; then
    IF=$1
fi

$RINACONF ipcp-create shim-eth e.IPCP 1
$RINACONF ipcp-config e.IPCP 1 netdev $IF
$RINACONF ipcp-config e.IPCP 1 dif e.DIF

source tests/epilogue.sh
