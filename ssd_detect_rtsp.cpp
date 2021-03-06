// This is a demo code for using a SSD model to do detection.
// The code is modified from examples/cpp_classification/classification.cpp.
// Usage:
//    ssd_detect [FLAGS] model_file weights_file list_file
//
// where model_file is the .prototxt file defining the network architecture, and
// weights_file is the .caffemodel file containing the network parameters, and
// list_file contains a list of image files with the format as follows:
//    folder/img1.JPEG
//    folder/img2.JPEG
// list_file can also contain a list of video files with the format as follows:
//    folder/video1.mp4
//    folder/video2.mp4
//
#include <caffe/caffe.hpp>
#ifdef USE_OPENCV
#include <opencv2/opencv.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/video/video.hpp>
#endif  // USE_OPENCV
#include <algorithm>
#include <iomanip>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef USE_OPENCV
using namespace caffe;  // NOLINT(build/namespaces)
using namespace cv;
using namespace std;

class Detector {
 public:
  Detector(const string& model_file,
           const string& weights_file,
           const string& mean_file,
           const string& mean_value);

  std::vector<vector<float> > Detect(const cv::Mat& img);

 private:
  void SetMean(const string& mean_file, const string& mean_value);

  void WrapInputLayer(std::vector<cv::Mat>* input_channels);

  void Preprocess(const cv::Mat& img,
                  std::vector<cv::Mat>* input_channels);

 private:
  shared_ptr<Net<float> > net_;
  cv::Size input_geometry_;
  int num_channels_;
  cv::Mat mean_;
};

Detector::Detector(const string& model_file,
                   const string& weights_file,
                   const string& mean_file,
                   const string& mean_value) {
#ifdef CPU_ONLY
  Caffe::set_mode(Caffe::CPU);
#else
  Caffe::set_mode(Caffe::GPU);
#endif

  /* Load the network. */
  net_.reset(new Net<float>(model_file, TEST));
  net_->CopyTrainedLayersFrom(weights_file);

  CHECK_EQ(net_->num_inputs(), 1) << "Network should have exactly one input.";
  CHECK_EQ(net_->num_outputs(), 1) << "Network should have exactly one output.";

  Blob<float>* input_layer = net_->input_blobs()[0];
  num_channels_ = input_layer->channels();
  CHECK(num_channels_ == 3 || num_channels_ == 1)
    << "Input layer should have 1 or 3 channels.";
  input_geometry_ = cv::Size(input_layer->width(), input_layer->height());

  /* Load the binaryproto mean file. */
  SetMean(mean_file, mean_value);
}

std::vector<vector<float> > Detector::Detect(const cv::Mat& img) {
  Blob<float>* input_layer = net_->input_blobs()[0];
  input_layer->Reshape(1, num_channels_,
                       input_geometry_.height, input_geometry_.width);
  /* Forward dimension change to all layers. */
  net_->Reshape();

  std::vector<cv::Mat> input_channels;
  WrapInputLayer(&input_channels);

  Preprocess(img, &input_channels);

  net_->Forward();

  /* Copy the output layer to a std::vector */
  Blob<float>* result_blob = net_->output_blobs()[0];
  const float* result = result_blob->cpu_data();
  const int num_det = result_blob->height();
  vector<vector<float> > detections;
  for (int k = 0; k < num_det; ++k) {
    if (result[0] == -1) {
      // Skip invalid detection.
      result += 7;
      continue;
    }
    vector<float> detection(result, result + 7);
    detections.push_back(detection);
    result += 7;
  }
  return detections;
}

/* Load the mean file in binaryproto format. */
void Detector::SetMean(const string& mean_file, const string& mean_value) {
  cv::Scalar channel_mean;
  if (!mean_file.empty()) {
    CHECK(mean_value.empty()) <<
      "Cannot specify mean_file and mean_value at the same time";
    BlobProto blob_proto;
    ReadProtoFromBinaryFileOrDie(mean_file.c_str(), &blob_proto);

    /* Convert from BlobProto to Blob<float> */
    Blob<float> mean_blob;
    mean_blob.FromProto(blob_proto);
    CHECK_EQ(mean_blob.channels(), num_channels_)
      << "Number of channels of mean file doesn't match input layer.";

    /* The format of the mean file is planar 32-bit float BGR or grayscale. */
    std::vector<cv::Mat> channels;
    float* data = mean_blob.mutable_cpu_data();
    for (int i = 0; i < num_channels_; ++i) {
      /* Extract an individual channel. */
      cv::Mat channel(mean_blob.height(), mean_blob.width(), CV_32FC1, data);
      channels.push_back(channel);
      data += mean_blob.height() * mean_blob.width();
    }

    /* Merge the separate channels into a single image. */
    cv::Mat mean;
    cv::merge(channels, mean);

    /* Compute the global mean pixel value and create a mean image
     * filled with this value. */
    channel_mean = cv::mean(mean);
    mean_ = cv::Mat(input_geometry_, mean.type(), channel_mean);
  }
  if (!mean_value.empty()) {
    CHECK(mean_file.empty()) <<
      "Cannot specify mean_file and mean_value at the same time";
    stringstream ss(mean_value);
    vector<float> values;
    string item;
    while (getline(ss, item, ',')) {
      float value = std::atof(item.c_str());
      values.push_back(value);
    }
    CHECK(values.size() == 1 || values.size() == num_channels_) <<
      "Specify either 1 mean_value or as many as channels: " << num_channels_;

    std::vector<cv::Mat> channels;
    for (int i = 0; i < num_channels_; ++i) {
      /* Extract an individual channel. */
      cv::Mat channel(input_geometry_.height, input_geometry_.width, CV_32FC1,
          cv::Scalar(values[i]));
      channels.push_back(channel);
    }
    cv::merge(channels, mean_);
  }
}

/* Wrap the input layer of the network in separate cv::Mat objects
 * (one per channel). This way we save one memcpy operation and we
 * don't need to rely on cudaMemcpy2D. The last preprocessing
 * operation will write the separate channels directly to the input
 * layer. */
void Detector::WrapInputLayer(std::vector<cv::Mat>* input_channels) {
  Blob<float>* input_layer = net_->input_blobs()[0];

  int width = input_layer->width();
  int height = input_layer->height();
  float* input_data = input_layer->mutable_cpu_data();
  for (int i = 0; i < input_layer->channels(); ++i) {
    cv::Mat channel(height, width, CV_32FC1, input_data);
    input_channels->push_back(channel);
    input_data += width * height;
  }
}

void Detector::Preprocess(const cv::Mat& img,
                            std::vector<cv::Mat>* input_channels) {
  /* Convert the input image to the input image format of the network. */
  cv::Mat sample;
  if (img.channels() == 3 && num_channels_ == 1)
    cv::cvtColor(img, sample, cv::COLOR_BGR2GRAY);
  else if (img.channels() == 4 && num_channels_ == 1)
    cv::cvtColor(img, sample, cv::COLOR_BGRA2GRAY);
  else if (img.channels() == 4 && num_channels_ == 3)
    cv::cvtColor(img, sample, cv::COLOR_BGRA2BGR);
  else if (img.channels() == 1 && num_channels_ == 3)
    cv::cvtColor(img, sample, cv::COLOR_GRAY2BGR);
  else
    sample = img;

  cv::Mat sample_resized;
  if (sample.size() != input_geometry_)
    cv::resize(sample, sample_resized, input_geometry_);
  else
    sample_resized = sample;

  cv::Mat sample_float;
  if (num_channels_ == 3)
    sample_resized.convertTo(sample_float, CV_32FC3);
  else
    sample_resized.convertTo(sample_float, CV_32FC1);

  cv::Mat sample_normalized;
  cv::subtract(sample_float, mean_, sample_normalized);

  /* This operation will write the separate BGR planes directly to the
   * input layer of the network because it is wrapped by the cv::Mat
   * objects in input_channels. */
  cv::split(sample_normalized, *input_channels);

  CHECK(reinterpret_cast<float*>(input_channels->at(0).data)
        == net_->input_blobs()[0]->cpu_data())
    << "Input channels are not wrapping the input layer of the network.";
}

class RTSP_Stream {
 public:
  RTSP_Stream(){};

  /*RTSP_Stream(const string& ip,
           const string& type,
           const string& channel);
 */

  int Config(const string& username, 
            const string& password, 
            const string& ip, 
            const string& type, 
            const string& channel);

  void Open();

  void Init(); 

  void GetFrame (cv::Mat& img);

 private:
  void Release();

  void GetConfig();

  void Display ();

 private:
  string source ;
  //shared_ptr<Net<float> > net_;
  //cv::Size input_geometry_;
  int num_channels_;
  cv::Mat mean_;
  VideoCapture cap;
};

void RTSP_Stream::Init() { 
  GetConfig(); 
}

void RTSP_Stream::GetConfig() {

  //std::ifstream infile("/etc/textile.conf");
  std::ifstream cfgfile("/home/orange/config/textile.conf");
  std::string rtsp_url = "rtsp://";
  std::string item;

  source = "rtsp://admin:a1234567@192.168.0.101/h264/ch1/sub/av_stream";

  //user name
  cfgfile >> item;
  cout << item  << std::endl;
  rtsp_url = rtsp_url + item + ":";

  //password
  cfgfile >> item;
  cout << item  << std::endl;
  rtsp_url = rtsp_url + item + "@";

  //ip
  cfgfile >> item;
  cout << item  << std::endl;
  rtsp_url = rtsp_url + item + "/h264/ch1/sub/av_stream";
  cout << rtsp_url  << std::endl;

  if (!rtsp_url.empty()) {
    source = rtsp_url;
  }

  return ;
}

void RTSP_Stream::Open(){
  cap.open(source);
  if(!cap.isOpened())
  {
    cout << "Can't open the stream: " << source << std::endl;
  }

  //cap.set(CV_CAP_PROP_FRAME_HEIGHT, 768);
  //cap.set(CV_CAP_PROP_FRAME_WIDTH, 1024);

  return ;
}

void RTSP_Stream::GetFrame(cv::Mat& img){
  
  cap >> img;
  if (img.empty())
  {
    cout << "Can't get frame: " << source << std::endl;
  }

  //img = _CImg;
  //push 

}

DEFINE_string(mean_file, "",
    "The mean file used to subtract from the input image.");
DEFINE_string(mean_value, "104,117,123",
    "If specified, can be one value or can be same as image channels"
    " - would subtract from the corresponding channel). Separated by ','."
    "Either mean_file or mean_value should be provided, not both.");
DEFINE_string(file_type, "image",
    "The file type in the list_file. Currently support image and video.");
DEFINE_string(out_file, "",
    "If provided, store the detection results in the out_file.");
DEFINE_double(confidence_threshold, 0.01,
    "Only store detections with score higher than the threshold.");

std::string& trim(std::string &); 

std::string& trim(std::string &s)   
{  
    if (s.empty())   
    {  
        return s;  
    }  
     s.erase(0,s.find_first_not_of(" "));  
     s.erase(s.find_last_not_of(" ") + 1);  
    return s;  
}  

void getAlgConf ()
{
  std::ifstream cfgfile("/home/orange/config/algconf.conf");

  string confidence_threshold ;
  string file_type ;
  string model_file ;
  string weights_file ;
  string list_file ;

  string item ;
  string prefix_str;
  int split_pos = 0;

  while ( !getline(cfgfile, item).eof()) {

    split_pos = item.find('=');
    prefix_str = item.substr(0, split_pos);
    prefix_str = trim (prefix_str);

    //cout << prefix_str <<std::endl;
    if (prefix_str.compare("threshold") == 0){
      confidence_threshold = item.substr(split_pos + 1, -1);
      cout << trim(confidence_threshold) <<std::endl;
    }
    else if (prefix_str.compare("type") == 0){
      file_type = item.substr(split_pos + 1, -1);
      file_type = trim(file_type);
      cout << file_type <<std::endl;
    }
    else if (prefix_str.compare("model") == 0){
      model_file = item.substr(split_pos + 1, -1);
      model_file = trim(model_file);
      cout << model_file <<std::endl;
    }
    else if (prefix_str.compare("data") == 0){
      weights_file = item.substr(split_pos + 1, -1);
      weights_file = trim(weights_file);
      cout << weights_file <<std::endl;
    }
    else if (prefix_str.compare("listfile") == 0){
      list_file = item.substr(split_pos + 1, -1);
      list_file = trim (list_file);
    }
  }
  
}

int main(int argc, char** argv) {
  ::google::InitGoogleLogging(argv[0]);
  // Print output to stderr (while still logging)
  FLAGS_alsologtostderr = 1;

#ifndef GFLAGS_GFLAGS_H_
  namespace gflags = google;
#endif

  gflags::SetUsageMessage("Do detection using SSD mode.\n"
        "Usage:\n"
        "    ssd_detect [FLAGS] model_file weights_file list_file\n");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  getAlgConf ();

  if (argc < 4) {
    gflags::ShowUsageWithFlagsRestrict(argv[0], "examples/ssd/ssd_detect");
    return 1;
  }

  const string& model_file = argv[1];
  const string& weights_file = argv[2];

  const string& mean_file = FLAGS_mean_file;
  const string& mean_value = FLAGS_mean_value;
  const string& file_type = FLAGS_file_type;
  const string& out_file = FLAGS_out_file;
  const float confidence_threshold = FLAGS_confidence_threshold;

  // Initialize the network.
  Detector detector(model_file, weights_file, mean_file, mean_value);

  // Set the output mode.
  std::streambuf* buf = std::cout.rdbuf();
  std::ofstream outfile;
  if (!out_file.empty()) {
    outfile.open(out_file.c_str());
    if (outfile.good()) {
      buf = outfile.rdbuf();
    }
  }
  std::ostream out(buf);

  // Process image one by one.
  std::ifstream infile(argv[3]);
  std::string file;
  while (infile >> file) {
    out <<"Debug: file :" <<file << std::endl;
    out <<"file type: " << file_type << std::endl;

    std::string prefix_str = file.substr(0, file.find(':'));
    cout << "Head : " << prefix_str << std::endl;
    
    if (prefix_str.compare("rtsp") == 0){
      cout << "opening the rtsp stream ..." << std::endl;
      
      //Using Opencv 3.0 
      #ifdef OPENCV_V2
      lplImage *pFrame = NULL, *srcImage = NULL;
      cvCapture *pCapture = NULL;
      pCapture = cvCreateFileCapture("rtsp://0.0.0.1/stream1");
      if(!pCapture){
        LOG(FATAL) << "Can not get the video stream from the camera!\n"
        return 0;
      }

      double rate = cvGetCaptureProperty(pCapture, CV_CAP_PROP_FPS);
      cvSiez size = cvSize((int)cvGetCaptureProperty(pCapture, CV_CAP_PROP_FRAME_WIDTH), 
        (int)cvGetCaptureProperty(pCapture, CV_CAP_PROP_FRAME_HEIGHT));
      cvVideoWriter *writer = cvCreateVideoWriter("videotest.avi", CV_FOURCC('M','J','P','G'), rate, size);
      while(1){
        pFrame = cvQueryFrame(pCapture);
        srcImage = cvCloneImage(pFrame);

        cvWriteFrame(writer, srcImage);
        cvShowImage("123234", srcImage);

        cvWaitKey(20);
        cvReleaseImage(&srcImage);
        srcImage = NULL;
      }
      cvReleaseCapture(&pCapture);
      cvReleaseImage(&pFrame);
      return 0
      #else //opencv 3 
      #if 0
      VideoCapture cap;
      //string source = "rtsp://admin:12345@192.168.1.64/Streaming/Channels/1";
      //string source = "rtsp://192.168.0.107:8554/1";
      string source = "rtsp://admin:a1234567@192.168.0.101/h264/ch1/sub/av_stream";

      if (!file.empty()) {
        source = file;
      }

      cap.open(source);

      Mat Camera_CImg;
      Mat Camera_GImg;
      //cap.set(CV_CAP_PROP_FRAME_HEIGHT, 768);
      //cap.set(CV_CAP_PROP_FRAME_WIDTH, 1024);

      if(!cap.isOpened())
      {
        cout << "Can't open the stream: " << source << std::endl;
        return 0;
      }

      while(1){
        cap >> Camera_CImg;
        if (Camera_CImg.empty())
          break;
        
        //cvtColor(Camera_CImg, Camera_GImg, CV_RGB2GRAY);
        std::vector<vector<float> > detections = detector.Detect(Camera_CImg);

        /* Print the detection results. */
        for (int i = 0; i < detections.size(); ++i) {
          const vector<float>& d = detections[i];
          // Detection format: [image_id, label, score, xmin, ymin, xmax, ymax].
          CHECK_EQ(d.size(), 7);
          const float score = d[2];
          if (score >= confidence_threshold) {
            out << file << " ";
            out << static_cast<int>(d[1]) << " ";
            out << score << " ";
            out << static_cast<int>(d[3] * Camera_CImg.cols) << " ";
            out << static_cast<int>(d[4] * Camera_CImg.rows) << " ";
            out << static_cast<int>(d[5] * Camera_CImg.cols) << " ";
            out << static_cast<int>(d[6] * Camera_CImg.rows) << std::endl;
          }
        }

        imshow("input", Camera_CImg);
        if(cvWaitKey(10) == 'q')
          break;
      }
      //system("pause");
      #else
      RTSP_Stream rtsp_stream;
      cv::Mat Camera_CImg;

      rtsp_stream.Init();
      rtsp_stream.Open();

      while(1){
        rtsp_stream.GetFrame(Camera_CImg);

        std::vector<vector<float> > detections = detector.Detect(Camera_CImg);

        /* Print the detection results. */
        for (int i = 0; i < detections.size(); ++i) {
          const vector<float>& d = detections[i];
          // Detection format: [image_id, label, score, xmin, ymin, xmax, ymax].
          CHECK_EQ(d.size(), 7);
          const float score = d[2];
          if (score >= confidence_threshold) {
            out << file << " ";
            out << static_cast<int>(d[1]) << " ";
            out << score << " ";
            out << static_cast<int>(d[3] * Camera_CImg.cols) << " ";
            out << static_cast<int>(d[4] * Camera_CImg.rows) << " ";
            out << static_cast<int>(d[5] * Camera_CImg.cols) << " ";
            out << static_cast<int>(d[6] * Camera_CImg.rows) << std::endl;
          }
        }

        imshow("input", Camera_CImg);
        if(cvWaitKey(10) == 'q')
          break;

      }


      #endif

      #endif
    }
    else if (file_type == "image") {
      cv::Mat img = cv::imread(file, -1);
      CHECK(!img.empty()) << "Unable to decode image " << file;
      std::vector<vector<float> > detections = detector.Detect(img);

      /* Print the detection results. */
      for (int i = 0; i < detections.size(); ++i) {
        const vector<float>& d = detections[i];
        // Detection format: [image_id, label, score, xmin, ymin, xmax, ymax].
        CHECK_EQ(d.size(), 7);
        const float score = d[2];
        if (score >= confidence_threshold) {
          out << file << " ";
          out << static_cast<int>(d[1]) << " ";
          out << score << " ";
          out << static_cast<int>(d[3] * img.cols) << " ";
          out << static_cast<int>(d[4] * img.rows) << " ";
          out << static_cast<int>(d[5] * img.cols) << " ";
          out << static_cast<int>(d[6] * img.rows) << std::endl;
        }
      }
    } else if (file_type == "video") {
      cv::VideoCapture cap(file);
      if (!cap.isOpened()) {
        LOG(FATAL) << "Failed to open video: " << file;
      }
      cv::Mat img;
      int frame_count = 0;
      while (true) {
        bool success = cap.read(img);
        if (!success) {
          LOG(INFO) << "Process " << frame_count << " frames from " << file;
          break;
        }
        CHECK(!img.empty()) << "Error when read frame";
        std::vector<vector<float> > detections = detector.Detect(img);

        /* Print the detection results. */
        for (int i = 0; i < detections.size(); ++i) {
          const vector<float>& d = detections[i];
          // Detection format: [image_id, label, score, xmin, ymin, xmax, ymax].
          CHECK_EQ(d.size(), 7);
          const float score = d[2];
          if (score >= confidence_threshold) {
            out << file << "_";
            out << std::setfill('0') << std::setw(6) << frame_count << " ";
            out << static_cast<int>(d[1]) << " ";
            out << score << " ";
            out << static_cast<int>(d[3] * img.cols) << " ";
            out << static_cast<int>(d[4] * img.rows) << " ";
            out << static_cast<int>(d[5] * img.cols) << " ";
            out << static_cast<int>(d[6] * img.rows) << std::endl;
          }
        }
        ++frame_count;
      }
      if (cap.isOpened()) {
        cap.release();
      }
    } 
    else {
      LOG(FATAL) << "Unknown file_type: " << file_type;
    }
  }
  return 0;
}
#else
int main(int argc, char** argv) {
  LOG(FATAL) << "This example requires OpenCV; compile with USE_OPENCV.";



}
#endif  // USE_OPENCV
