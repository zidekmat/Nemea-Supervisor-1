#!/bin/bash
#Checks free memory.
#If it is less than 10% of free memory it returns 2 as critical.

TOTAL=$(cat /proc/meminfo | grep MemTotal | tr -s ' ' | cut -d' ' -f2)
FREE=$(cat /proc/meminfo | grep MemFree | tr -s ' ' | cut -d' ' -f2)

#PERCENT of total memory used
PERCENT=$( echo "scale=2; ($FREE/$TOTAL)*100" | bc | cut -d'.' -f1)

if [ "$PERCENT" -lt 90 ]; then
   echo "Memory OK."
   exit 0
fi
echo "Running out. Less than 10% free memory."
exit 2
