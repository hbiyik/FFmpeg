source definitions.sh

TESTNAME=encoder_planes_drm

foreachencoderformat 'ffmpeg -t 1 -init_hw_device drm=dr:/dev/dri/renderD128 -filter_hw_device dr -f lavfi -i testsrc=s=1920x1080,format=$format -vf hwupload,format=drm_prime -c:v ${encoder}_rkmpp_encoder -y ${testpath}/${encoder}_${format}.${!ext}'
