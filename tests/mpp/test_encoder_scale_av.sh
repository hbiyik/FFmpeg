source definitions.sh

TESTNAME=encoder_scale_av

foreachencoderformat 'ffmpeg -t 1 -f lavfi -i testsrc=s=1920x1080:r=30,format=${format} -loglevel info -c:v ${encoder}_rkmpp_encoder -width 1280 -height 720 -y ${testpath}/${encoder}_${format}.${!ext}'
