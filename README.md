# gst-webrtc-example

# Initial setup of simple Javascript webpage and Signalling server

This can be done with same example https://github.com/centricular/gstwebrtc-demos which is used a reference for this demo.
Thanks to the Centricular Team for this support

Please run the webapp and note down the peer id generated, say example, 1232

# Building binary with cmake
cd */gst-webrtc-example
mkdir cmake-build-debug
cd cmake-build-debug
cmake ..
make

# Check with ls for binary as below
rtsp2webrtc_1_n

# For recording .mp4 videos
Create folder 'mkdir /mnt/av/ ' with write permissions
Or update const variable 'BASE_RECORDING_PATH' in file rtsp_webrtc_1_n.cpp accordingly

# Running binary
./rtsp2webrtc_1_n <SIGNALLING SERVER URL> <PEER ID NOTED FROM BROWSER> <PCAP FILE PATH> <RTP SOURCE IP> <RTP SOURCE PORT>

Example: ./rtsp2webrtc_1_n wss://127.0.0.1:8443 1232 /home/user/pcap_recording_webrtc_stuttering_10_56598.pcap 192.168.0.10 56598

And check logs, video for further analysis



