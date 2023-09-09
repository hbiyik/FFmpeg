source definitions.sh

TESTNAME=encoder_oddaling_av

foreachencoderformat 'ffmpeg -t 1 -f lavfi -i testsrc=s=720x480:r=30,format=${format} -loglevel info -c:v ${encoder}_rkmpp_encoder -y ${testpath}/${encoder}_${format}.${!ext}'
