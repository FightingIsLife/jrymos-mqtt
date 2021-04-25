#!/bin/bash

echo 'clone submodule code'
git submodule update --init --recursive

echo 'copy code to backup'
mkdir backup
cp -r mosquitto/ backup/
cp -r mosquitto-auth-plug/ backup/

echo 'replace code'
cp -r src/* backup/mosquitto-auth-plug/
cd backup/mosquitto-auth-plug/
echo 'make'
make clean
make
echo 'copy auth-plug.so'
version=$(lsb_release -r | awk -F ':' '{print $2}' | awk -F '.'  '{print $1}' | sed s/[[:space:]]//g) 
plug_name='auth-plug-debian-'$version'.so'
echo $plug_name
cd ../..
mv backup/mosquitto-auth-plug/auth-plug.so ./$plug_name
echo 'end'
