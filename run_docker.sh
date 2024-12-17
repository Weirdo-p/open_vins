xhost +local:docker

sudo docker run --privileged -it \
           -e NVIDIA_DRIVER_CAPABILITIES=all \
           -e NVIDIA_VISIBLE_DEVICES=all -e DISPLAY=$DISPLAY\
           --volume=./:/home/catkin_ws/src \
           --volume=/tmp/.X11-unix:/tmp/.X11-unix:rw \
           --net=host \
           --ipc=host \
           --shm-size=1gb \
           --name=open_vins \
           --env="DISPLAY=$DISPLAY" \
         open_vins:v1 /bin/bash

