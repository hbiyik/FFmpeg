source definitions.sh

TESTNAME=encoder_oddalign_drm_scale

foreachencoderformat 'ffmpeg -t 1 -init_hw_device drm=dr:/dev/dri/renderD128 -filter_hw_device dr -f lavfi -i testsrc=s=720x480,format=$format -vf hwupload,format=drm_prime -c:v ${encoder}_rkmpp_encoder -width 360 -height 240 -y ${testpath}/${encoder}_${format}.${!ext}'
