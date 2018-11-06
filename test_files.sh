#!/bin/bash

echo "starting test" 
for x in Tests/*
do
	kill $(ps aux | grep './receiver 9898 blahw.txt' | awk '{print $3}') 2>/dev/null
	start=$SECONDS
	./receiver 9898 ./outfile &
	./sender 127.0.0.1 9898 $x
	duration=$(( SECONDS - start))
	cmp $x ./outfile 
	if [ $? -ne 0 ] 
	then
		echo "$x ERROR!" 
		break
	fi 
	size=$(stat -c%s "$x")
	echo "$x = $size bytes"
	echo "$x PASSED!" 
	echo "$(md5sum $x) = $(md5sum ./outfile)"
	echo "Took $duration seconds" 
done

