#include "capture_publisher_input.h"
#include <libyuv.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace goodsharing {
namespace {
std::uint64_t nowUs(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct Fit {int width,height,left,top;};
Fit fitCanvas(int sourceWidth,int sourceHeight,int width,int height) {
    const double scale=std::min(static_cast<double>(width)/sourceWidth,static_cast<double>(height)/sourceHeight);
    const int fittedWidth=std::clamp(static_cast<int>(sourceWidth*scale)&~1,2,width);
    const int fittedHeight=std::clamp(static_cast<int>(sourceHeight*scale)&~1,2,height);
    return {fittedWidth,fittedHeight,((width-fittedWidth)/2)&~1,((height-fittedHeight)/2)&~1};
}
// Point sampling when enlarging keeps the previous Qt FastTransformation result
// (marker block centres are unchanged); area averaging when reducing keeps
// detail that point sampling would drop.
libyuv::FilterMode canvasFilter(const Fit& fit,int sourceWidth,int sourceHeight) {
    return fit.width<sourceWidth||fit.height<sourceHeight?libyuv::kFilterBox:libyuv::kFilterNone;
}
// Per-converting-thread scratch: the capture loop rebuilds these buffers for
// every frame, so keeping capacity avoids fresh page faults per frame.
std::array<std::vector<std::uint8_t>,2>& chromaScratch() {
    thread_local std::array<std::vector<std::uint8_t>,2> scratch;
    return scratch;
}
std::vector<std::uint8_t>& packedScratch(int index) {
    thread_local std::array<std::vector<std::uint8_t>,2> scratch;
    return scratch[static_cast<std::size_t>(index)];
}
}
void capturePlanesI420(const QImage& image,PlaneImageI420& output) {
    if(image.isNull()||image.width()>8192||image.height()>8192)throw std::invalid_argument("Invalid captured image");
    // Premultiplication composites transparent capture pixels over black
    // before the converter ignores alpha.
    QImage packed=image;
    if(packed.format()!=QImage::Format_ARGB32_Premultiplied&&packed.format()!=QImage::Format_ARGB32&&packed.format()!=QImage::Format_RGB32)
        packed=packed.convertToFormat(QImage::Format_ARGB32);
    const bool premultiplied=packed.format()==QImage::Format_ARGB32_Premultiplied;
    const auto* bits=packed.constBits();
    int stride=static_cast<int>(packed.bytesPerLine()),width=packed.width(),height=packed.height();
    // Bound the converted source without changing the previous aspect-preserving
    // limit; the canvas pass below still performs the final fit.
    if(width>1920||height>1080) {
        auto& capped=packedScratch(0);
        const auto fit=fitCanvas(width,height,1920,1080);
        capped.resize(static_cast<std::size_t>(fit.width)*fit.height*4);
        if(libyuv::ARGBScale(bits,stride,width,height,capped.data(),fit.width*4,fit.width,fit.height,canvasFilter(fit,width,height))!=0)
            throw std::runtime_error("Captured image downscale failed");
        bits=capped.data();stride=fit.width*4;width=fit.width;height=fit.height;
    }
    if(!premultiplied) {
        auto& attenuated=packedScratch(1);
        attenuated.resize(static_cast<std::size_t>(width)*height*4);
        if(libyuv::ARGBAttenuate(bits,stride,attenuated.data(),width*4,width,height)!=0)
            throw std::runtime_error("Captured image alpha composition failed");
        bits=attenuated.data();stride=width*4;
    }
    output.width=width;output.height=height;
    const int chromaWidth=(width+1)/2,chromaHeight=(height+1)/2;
    output.planes[0].resize(static_cast<std::size_t>(width)*height);
    output.planes[1].resize(static_cast<std::size_t>(chromaWidth)*chromaHeight);
    output.planes[2].resize(static_cast<std::size_t>(chromaWidth)*chromaHeight);
    if(libyuv::ARGBToI420(bits,stride,output.planes[0].data(),width,output.planes[1].data(),chromaWidth,output.planes[2].data(),chromaWidth,width,height)!=0)
        throw std::runtime_error("Captured image conversion failed");
}
std::vector<std::uint8_t> captureCanvasNv12(const PlaneImageI420& source,int width,int height) {
    if(source.width<2||source.height<2||width<2||height<2||width>1920||height>1080||width%2||height%2)
        throw std::invalid_argument("Invalid capture canvas");
    const auto fit=fitCanvas(source.width,source.height,width,height);
    const auto luma=static_cast<std::size_t>(width)*height;
    const bool covered=fit.left==0&&fit.top==0&&fit.width==width&&fit.height==height;
    std::vector<std::uint8_t> output(luma*3/2);
    if(!covered){std::fill_n(output.begin(),luma,16);std::fill(output.begin()+static_cast<std::ptrdiff_t>(luma),output.end(),128);}
    auto* destinationY=output.data()+static_cast<std::size_t>(fit.top)*width+fit.left;
    auto* destinationUV=output.data()+luma+static_cast<std::size_t>(fit.top/2)*width+fit.left;
    const int sourceChromaWidth=(source.width+1)/2,sourceChromaHeight=(source.height+1)/2;
    const int destinationChromaWidth=fit.width/2,destinationChromaHeight=fit.height/2;
    const auto* sourceY=source.planes[0].data();
    const auto* sourceU=source.planes[1].data();
    const auto* sourceV=source.planes[2].data();
    if(fit.width==source.width&&fit.height==source.height) {
        // Matching dimensions need no filtering: copy the rows straight into the
        // canvas instead of scaling (and copying) a same-size intermediate.
        libyuv::CopyPlane(sourceY,source.width,destinationY,width,fit.width,fit.height);
        libyuv::MergeUVPlane(sourceU,sourceChromaWidth,sourceV,sourceChromaWidth,destinationUV,width,destinationChromaWidth,destinationChromaHeight);
        return output;
    }
    const auto filter=canvasFilter(fit,source.width,source.height);
    if(libyuv::ScalePlane(sourceY,source.width,source.width,source.height,destinationY,width,fit.width,fit.height,filter)!=0)
        throw std::runtime_error("Captured image scaling failed");
    auto& scratch=chromaScratch();
    const auto chromaSize=static_cast<std::size_t>(destinationChromaWidth)*destinationChromaHeight;
    scratch[0].resize(chromaSize);scratch[1].resize(chromaSize);
    if(libyuv::ScalePlane(sourceU,sourceChromaWidth,sourceChromaWidth,sourceChromaHeight,scratch[0].data(),destinationChromaWidth,destinationChromaWidth,destinationChromaHeight,filter)!=0||
       libyuv::ScalePlane(sourceV,sourceChromaWidth,sourceChromaWidth,sourceChromaHeight,scratch[1].data(),destinationChromaWidth,destinationChromaWidth,destinationChromaHeight,filter)!=0)
        throw std::runtime_error("Captured image scaling failed");
    libyuv::MergeUVPlane(scratch[0].data(),destinationChromaWidth,scratch[1].data(),destinationChromaWidth,destinationUV,width,destinationChromaWidth,destinationChromaHeight);
    return output;
}
std::vector<std::uint8_t> captureCanvasNv12(const QImage& image,int width,int height) {
    PlaneImageI420 planes;capturePlanesI420(image,planes);
    return captureCanvasNv12(planes,width,height);
}
CapturePublisherInput::~CapturePublisherInput(){stop();}
void CapturePublisherInput::start(Config config,std::shared_ptr<ShareInput> input,Preview preview,Level level,SourceState state) {
    stop();
    if(!input||!input->gate()->ready()||!config.handle||(config.audio==AudioMode::Application&&!config.audioPid))
        throw std::invalid_argument("Capture requires a source and a ready encrypted publication");
    LARGE_INTEGER counter,frequency;
    if(!QueryPerformanceFrequency(&frequency)||!QueryPerformanceCounter(&counter))throw std::runtime_error("Capture clock unavailable");
    clockOffsetUs_=static_cast<std::int64_t>(nowUs())-static_cast<std::int64_t>(static_cast<long double>(counter.QuadPart)*1000000/frequency.QuadPart);
    {std::lock_guard lock(mutex_);input_=std::move(input);latest_.reset();error_.clear();captured_=replaced_=stale_=videoAccepted_=audioAccepted_=conversionUs_=maxConversionUs_=0;
        sourceConversionUs_=canvas1080Us_=canvas720Us_=convertedFrames_=sourceWidth_=sourceHeight_=0;sourceMinimized_=false;planes_=PlaneImageI420{};}
    audioPending_.clear();audioPendingUs_=lastAudioEndUs_=audioDiscontinuities_=0;
    audioSignalAbs_=audioDifferenceAbs_=0;level_=std::move(level);open_=true;
    converter_=std::jthread([this](std::stop_token stop){convert(stop);});
    const auto failure=[this](QString message){fail(std::move(message));};
    screen_.start(config.handle,config.monitor,[this,preview=std::move(preview)](ScreenFrame frame){
        if(!open_||!input_->gate()->ready())return;
        frame.timestampUs+=clockOffsetUs_;
        if(frame.timestampUs<=0){fail("Invalid screen capture timestamp");return;}
        if(preview)preview(frame);
        {std::lock_guard lock(mutex_);if(!open_)return;captured_++;if(latest_)replaced_++;latest_=std::move(frame);}
        ready_.notify_one();
    },failure,config.maximumFps>0?config.maximumFps:60,[this,state=std::move(state)](bool minimized){
        {std::lock_guard lock(mutex_);sourceMinimized_=minimized;}
        if(state)state(minimized);
    });
    audio_.start(config.audio,config.audioPid,[this](AudioBlock block){receiveAudio(std::move(block));},failure);
}
void CapturePublisherInput::fail(QString message) {
    if(!open_.exchange(false))return;
    if(input_){input_->gate()->close(true);input_->close();}
    {std::lock_guard lock(mutex_);if(error_.isEmpty())error_=std::move(message);latest_.reset();}
    ready_.notify_all();
}
void CapturePublisherInput::convert(std::stop_token stop) {
    try {
        std::uint32_t ordinal=0;
        while(!stop.stop_requested()) {
            ScreenFrame frame;
            {std::unique_lock lock(mutex_);ready_.wait(lock,stop,[this]{return !open_||latest_.has_value();});
             if(!open_||stop.stop_requested())break;frame=std::move(*latest_);latest_.reset();}
            if(!input_->gate()->ready())break;
            const auto began=nowUs(),capture=static_cast<std::uint64_t>(frame.timestampUs);
            if(capture>began+50000||began>capture+150000){std::lock_guard lock(mutex_);stale_++;continue;}
            ShareVideoInput next;next.identity={capture,++ordinal,false};
            // Both qualities share one I420 conversion. Scaling then copies the
            // fitted rows straight into each canvas.
            const auto sourceBegin=nowUs();
            capturePlanesI420(frame.image,planes_);
            const auto sourceEnd=nowUs();
            next.nv12[0]=captureCanvasNv12(planes_,1920,1080);
            const auto canvas1080End=nowUs();
            next.nv12[1]=captureCanvasNv12(planes_,1280,720);
            const auto canvas720End=nowUs();
            const auto duration=nowUs()-began;
            const bool accepted=open_&&input_->video(std::move(next));
            {std::lock_guard lock(mutex_);conversionUs_+=duration;maxConversionUs_=std::max(maxConversionUs_,duration);
                sourceConversionUs_+=sourceEnd-sourceBegin;canvas1080Us_+=canvas1080End-sourceEnd;canvas720Us_+=canvas720End-canvas1080End;
                sourceWidth_=static_cast<std::uint32_t>(planes_.width);sourceHeight_=static_cast<std::uint32_t>(planes_.height);convertedFrames_++;
                if(accepted)videoAccepted_++;else stale_++;}
        }
    }catch(const std::exception&){fail("Screen conversion failed; share stopped");}
}
void CapturePublisherInput::receiveAudio(AudioBlock block) {
    if(!open_||!input_->gate()->ready())return;
    if(block.sampleRate!=48000||block.channels!=2||block.samples.empty()||block.samples.size()%2||block.samples.size()>48000*2/10||block.timestampUs<=0){fail("Invalid captured audio block");return;}
    double signalAbs=0,differenceAbs=0;
    for(std::size_t i=0;i+1<block.samples.size();i+=2){
        const auto left=static_cast<double>(block.samples[i]),right=static_cast<double>(block.samples[i+1]);
        signalAbs+=(std::abs(left)+std::abs(right))/2;
        differenceAbs+=std::abs(left-right);
    }
    {std::lock_guard lock(mutex_);audioSignalAbs_+=signalAbs;audioDifferenceAbs_+=differenceAbs;}
    const auto timestamp=block.timestampUs+clockOffsetUs_;
    if(timestamp<=0){fail("Invalid audio capture timestamp");return;}
    const auto at=static_cast<std::uint64_t>(timestamp),frames=block.samples.size()/2;
    if(level_){double sum=0;for(auto sample:block.samples)sum+=static_cast<double>(sample)*sample;level_(std::sqrt(sum/block.samples.size())/32768.);}
    // Never concatenate samples across a capture discontinuity. A partial
    // (<10 ms) old block is discarded, with the original new timestamp kept.
    if(lastAudioEndUs_&&(at+2000<lastAudioEndUs_||at>lastAudioEndUs_+2000)){audioPending_.clear();audioPendingUs_=0;audioDiscontinuities_++;}
    lastAudioEndUs_=at+frames*1000000/48000;
    if(audioPending_.empty())audioPendingUs_=at;
    audioPending_.insert(audioPending_.end(),block.samples.begin(),block.samples.end());
    while(audioPending_.size()>=960) {
        ShareAudioInput next{audioPendingUs_,{audioPending_.begin(),audioPending_.begin()+960}};
        const bool accepted=open_&&input_->audio(std::move(next));
        {std::lock_guard lock(mutex_);if(accepted)audioAccepted_++;}
        audioPending_.erase(audioPending_.begin(),audioPending_.begin()+960);audioPendingUs_+=10000;
        if(!accepted){if(open_&&!input_->gate()->closed())fail("Captured audio rejected; share stopped");return;}
    }
}
void CapturePublisherInput::stop() {
    open_=false;if(input_)input_->close();ready_.notify_all();
    screen_.stop();audio_.stop();
    if(converter_.joinable()){converter_.request_stop();ready_.notify_all();converter_.join();}
    {std::lock_guard lock(mutex_);latest_.reset();}audioPending_.clear();
}
QString CapturePublisherInput::error()const{std::lock_guard lock(mutex_);return error_;}
QJsonObject CapturePublisherInput::report()const {
    std::lock_guard lock(mutex_);
    return {{"open",open_.load()},{"error",error_},{"captured_frames",static_cast<double>(captured_)},
        {"raw_replaced",static_cast<double>(replaced_)},{"stale_or_rejected_video",static_cast<double>(stale_)},
        {"video_pairs_admitted",static_cast<double>(videoAccepted_)},{"audio_blocks_admitted",static_cast<double>(audioAccepted_)},
        {"audio_channel_difference_ratio",audioSignalAbs_>0?audioDifferenceAbs_/audioSignalAbs_:0},
        {"conversion_total_ms",conversionUs_/1000.},{"maximum_conversion_ms",maxConversionUs_/1000.},
        {"source_conversion_average_ms",convertedFrames_?sourceConversionUs_/1000.0/convertedFrames_:0},
        {"canvas_1080_average_ms",convertedFrames_?canvas1080Us_/1000.0/convertedFrames_:0},
        {"canvas_720_average_ms",convertedFrames_?canvas720Us_/1000.0/convertedFrames_:0},
        {"converted_frames",static_cast<double>(convertedFrames_)},
        {"source_width",static_cast<double>(sourceWidth_)},{"source_height",static_cast<double>(sourceHeight_)},{"source_minimized",sourceMinimized_},
        {"pending_raw_frames",latest_?1:0},{"clock_offset_us",static_cast<double>(clockOffsetUs_)},{"gpu_conversion",false}};
}
}

