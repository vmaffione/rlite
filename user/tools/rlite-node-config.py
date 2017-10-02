#!/usr/bin/env python

import subprocess
import argparse
import re

description = "Simple configuration tool for rlite"
epilog = "2017 Vincenzo Maffione <v.maffione@gmail.com>"

argparser = argparse.ArgumentParser(description = description,
                                    epilog = epilog)
argparser.add_argument('-s', '--script',
                       help = "Path of the script file to be run",
                       type = str, default = '/etc/rina/initscript')

args = argparser.parse_args()

try:
    fin = open(args.script, "r")
except:
    print("Failed to open script file %s" % args.script)
    quit()

cmds = []

linecnt = 0
while 1:
    line = fin.readline()
    if line == "":
        break
    linecnt += 1

    line = line.replace('\n', '').strip()
    if line.startswith("#"):
        continue

    splitted = line.split()
    if splitted[0] == 'ipcp-enroll':
        enrolls.append((linecnt, line))
    else:
        cmds.append((linecnt, line))

fin.close()

for cmd in cmds:
    cmdline = 'rlite-ctl %s' % cmd[1]
    try:
        subprocess.check_call(cmdline)
    except Exception as e:
        print("Failure at line %s --> %s" % (cmd[0], e))
        quit()

for cmd in enrolls:
    cmdline = 'rlite-ctl %s' % cmd[1]
    try:
        subprocess.check_call(cmdline)
    except Exception as e:
        print("Failure at line %s --> %s" % (cmd[0], e))
        quit()
