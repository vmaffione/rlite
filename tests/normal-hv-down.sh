#!/bin/bash

source tests/prologue.sh
source tests/env.sh

if [ "$1" != "g" -a "$1" != "h" ]; then
    echo "First argument required ('h' or 'g')"
    exit 1
fi

if [ "$1" == "h" ]; then
    SHIPCP="h.IPCP"
    NORMIPCP="nh.IPCP"
else
    SHIPCP="g.IPCP"
    NORMIPCP="ng.IPCP"
fi

# it's not necessary to unregister, we can rely on
# auto-unregistration
rina-config ipcp-destroy $NORMIPCP 1
rina-config ipcp-destroy $SHIPCP 1

source tests/epilogue.sh
