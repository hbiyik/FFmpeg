vp8_formats=(nv16 yuv422p yuyv422 uyvy422 rgba rgb0 bgra bgr0 nv12 yuv420p)
h264_formats=(nv24 yuv444p nv16 yuv422p bgr24 yuyv422 uyvy422 rgba rgb0 bgra bgr0 nv12 yuv420p)
hevc_formats=${h264_formats[@]}

vp8_ext=webm
h264_ext=mp4
hevc_ext=$h264_ext

encoders=(hevc h264 vp8)
resultdir=results

resetdir(){
    mkdir -p $1
    rm -rf $1/*
}

foreachencoderformat(){
    testpath=$resultdir/$TESTNAME
    resetdir $testpath
    for encoder in ${encoders[@]}
    do
      formats=${encoder}_formats[@]
      ext=${encoder}_ext
      for format in ${!formats}
      do
        echo $1
        eval $1
      done
    done
}
