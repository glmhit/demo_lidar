#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <ros/ros.h>

#include "cameraParameters.h"
#include "pointDefinition.h"

using namespace std;
using namespace cv;

bool systemInited = false;
double timeCur, timeLast;

const int imagePixelNum = imageHeight * imageWidth;
CvSize imgSize = cvSize(imageWidth, imageHeight);

IplImage *imageCur = cvCreateImage(imgSize, IPL_DEPTH_8U, 1);
IplImage *imageLast = cvCreateImage(imgSize, IPL_DEPTH_8U, 1);

int showCount = 0;
const int showSkipNum = 2;
const int showDSRate = 3;
CvSize showSize = cvSize(imageWidth / showDSRate, imageHeight / showDSRate);

IplImage *imageShowMono = cvCreateImage(showSize, IPL_DEPTH_8U, 1);
IplImage *imageShowRGB = cvCreateImage(showSize, IPL_DEPTH_8U, 3);

CvMat kMat = cvMat(3, 3, CV_64FC1, kArray);
CvMat dMat = cvMat(4, 1, CV_64FC1, dArray);

IplImage *mapx, *mapy;

const int maxFeatureNumPerSubregion = 20;
const int xSubregionNum = 6;
const int ySubregionNum = 4;
const int totalSubregionNum = xSubregionNum * ySubregionNum;
const int MAXFEATURENUM = maxFeatureNumPerSubregion * totalSubregionNum;

const int xBoundary = 30;
const int yBoundary = 30;
const double subregionWidth = (double)(imageWidth - 2 * xBoundary) / (double)xSubregionNum;
const double subregionHeight = (double)(imageHeight - 2 * yBoundary) / (double)ySubregionNum;

const double maxTrackDis = 10;
const int winSize = 21;

IplImage *imageEig, *imageTmp, *pyrCur, *pyrLast;

CvPoint2D32f *featuresCur = new CvPoint2D32f[2 * MAXFEATURENUM];
CvPoint2D32f *featuresLast = new CvPoint2D32f[2 * MAXFEATURENUM];
char featuresFound[2 * MAXFEATURENUM];
float featuresError[2 * MAXFEATURENUM];

int featuresIndFromStart = 0;
int featuresInd[2 * MAXFEATURENUM] = {0};

int totalFeatureNum = 0;
int subregionFeatureNum[2 * totalSubregionNum] = {0};

pcl::PointCloud<ImagePoint>::Ptr imagePointsCur(new pcl::PointCloud<ImagePoint>());
pcl::PointCloud<ImagePoint>::Ptr imagePointsLast(new pcl::PointCloud<ImagePoint>());

ros::Publisher *imagePointsLastPubPointer;
ros::Publisher *imageShowPubPointer;
sensor_msgs::CvBridge bridge;

void imageDataHandler(const sensor_msgs::Image::ConstPtr& imageData) 
{
  timeLast = timeCur;
  timeCur = imageData->header.stamp.toSec();

  IplImage *imageTemp = imageLast;
  imageLast = imageCur;
  imageCur = imageTemp;

  for (int i = 0; i < imagePixelNum; i++) {
    imageCur->imageData[i] = (char)imageData->data[i];
  }

  IplImage *t = cvCloneImage(imageCur);
  cvRemap(t, imageCur, mapx, mapy);
  //cvEqualizeHist(imageCur, imageCur);
  cvReleaseImage(&t);

  CvPoint2D32f *featuresTemp = featuresLast;
  featuresLast = featuresCur;
  featuresCur = featuresTemp;

  pcl::PointCloud<ImagePoint>::Ptr imagePointsTemp = imagePointsLast;
  imagePointsLast = imagePointsCur;
  imagePointsCur = imagePointsTemp;
  imagePointsCur->clear();

  if (!systemInited) {
    systemInited = true;
    return;
  }

  int recordFeatureNum = totalFeatureNum;
  for (int i = 0; i < ySubregionNum; i++) {
    for (int j = 0; j < xSubregionNum; j++) {
      int ind = xSubregionNum * i + j;
      int numToFind = maxFeatureNumPerSubregion - subregionFeatureNum[ind];

      if (numToFind > 0) {
        int subregionLeft = xBoundary + (int)(subregionWidth * j);
        int subregionTop = yBoundary + (int)(subregionHeight * i);
        CvRect subregion = cvRect(subregionLeft, subregionTop, (int)subregionWidth, (int)subregionHeight);
        cvSetImageROI(imageLast, subregion);

        cvGoodFeaturesToTrack(imageLast, imageEig, imageTmp, featuresLast + totalFeatureNum,
                              &numToFind, 0.1, 5.0, NULL, 3, 1, 0.04);

        for(int k = 0; k < numToFind; k++) {
          featuresLast[totalFeatureNum + k].x += subregionLeft;
          featuresLast[totalFeatureNum + k].y += subregionTop;
          featuresInd[totalFeatureNum + k] = featuresIndFromStart;
          featuresIndFromStart++;
        }
        totalFeatureNum += numToFind;
        subregionFeatureNum[ind] += numToFind;

        cvResetImageROI(imageLast);
      }
    }
  }

  cvCalcOpticalFlowPyrLK(imageLast, imageCur, pyrLast, pyrCur,
                         featuresLast, featuresCur, totalFeatureNum, cvSize(winSize, winSize), 
                         3, featuresFound, featuresError, 
                         cvTermCriteria(CV_TERMCRIT_ITER | CV_TERMCRIT_EPS, 30, 0.01), 0);

  for (int i = 0; i < totalSubregionNum; i++) {
    subregionFeatureNum[i] = 0;
  }

  ImagePoint point;
  int featureCount = 0;
  double meanShiftX = 0, meanShiftY = 0;
  for (int i = 0; i < totalFeatureNum; i++) {
    double trackDis = sqrt((featuresLast[i].x - featuresCur[i].x) 
                    * (featuresLast[i].x - featuresCur[i].x)
                    + (featuresLast[i].y - featuresCur[i].y) 
                    * (featuresLast[i].y - featuresCur[i].y));

    if (!(trackDis > maxTrackDis || featuresCur[i].x < xBoundary || 
      featuresCur[i].x > imageWidth - xBoundary || featuresCur[i].y < yBoundary || 
      featuresCur[i].y > imageHeight - yBoundary)) {

      int xInd = (int)((featuresLast[i].x - xBoundary) / subregionWidth);
      int yInd = (int)((featuresLast[i].y - yBoundary) / subregionHeight);
      int ind = xSubregionNum * yInd + xInd;

      if (subregionFeatureNum[ind] < maxFeatureNumPerSubregion) {
        featuresCur[featureCount].x = featuresCur[i].x;
        featuresCur[featureCount].y = featuresCur[i].y;
        featuresLast[featureCount].x = featuresLast[i].x;
        featuresLast[featureCount].y = featuresLast[i].y;
        featuresInd[featureCount] = featuresInd[i];

        point.u = -(featuresCur[featureCount].x - kArray[2]) / kArray[0];
        point.v = -(featuresCur[featureCount].y - kArray[5]) / kArray[4];
        point.ind = featuresInd[featureCount];
        imagePointsCur->push_back(point);

        if (i >= recordFeatureNum) {
          point.u = -(featuresLast[featureCount].x - kArray[2]) / kArray[0];
          point.v = -(featuresLast[featureCount].y - kArray[5]) / kArray[4];
          imagePointsLast->push_back(point);
        }

        meanShiftX += fabs((featuresCur[featureCount].x - featuresLast[featureCount].x) / kArray[0]);
        meanShiftY += fabs((featuresCur[featureCount].y - featuresLast[featureCount].y) / kArray[4]);

        featureCount++;
        subregionFeatureNum[ind]++;
      }
    }
  }
  totalFeatureNum = featureCount;
  meanShiftX /= totalFeatureNum;
  meanShiftY /= totalFeatureNum;

  imagePointsLast->header.stamp = ros::Time().fromSec(timeLast);
  sensor_msgs::PointCloud2 imagePointsLast2;
  pcl::toROSMsg(*imagePointsLast, imagePointsLast2);
  imagePointsLastPubPointer->publish(imagePointsLast2);

  showCount = (showCount + 1) % (showSkipNum + 1);
  if (showCount == showSkipNum) {
    cvResize(imageLast, imageShowMono);
    cvCvtColor(imageShowMono, imageShowRGB, CV_GRAY2RGB);

    /*for(int i = 0; i < totalFeatureNum; i++) {
      cvCircle(imageShowRGB, cvPoint(featuresCur[i].x / showDSRate, featuresCur[i].y / showDSRate), 
               1, CV_RGB(0, 0, 255), 2);
    }*/

    sensor_msgs::Image::Ptr imageShowPointer = bridge.cvToImgMsg(imageShowRGB, "bgr8");
    imageShowPubPointer->publish(imageShowPointer);
  }
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "featureTracking");
  ros::NodeHandle nh;

  mapx = cvCreateImage(imgSize, IPL_DEPTH_32F, 1);
  mapy = cvCreateImage(imgSize, IPL_DEPTH_32F, 1);
  cvInitUndistortMap(&kMat, &dMat, mapx, mapy);

  CvSize subregionSize = cvSize((int)subregionWidth, (int)subregionHeight);
  imageEig = cvCreateImage(subregionSize, IPL_DEPTH_32F, 1);
  imageTmp = cvCreateImage(subregionSize, IPL_DEPTH_32F, 1);

  CvSize pyrSize = cvSize(imageWidth + 8, imageHeight / 3);
  pyrCur = cvCreateImage(pyrSize, IPL_DEPTH_32F, 1);
  pyrLast = cvCreateImage(pyrSize, IPL_DEPTH_32F, 1);

  ros::Subscriber imageDataSub = nh.subscribe<sensor_msgs::Image>("/image/raw", 1, imageDataHandler);

  ros::Publisher imagePointsLastPub = nh.advertise<sensor_msgs::PointCloud2> ("/image_points_last", 5);
  imagePointsLastPubPointer = &imagePointsLastPub;

  ros::Publisher imageShowPub = nh.advertise<sensor_msgs::Image>("/image/show", 1);
  imageShowPubPointer = &imageShowPub;

  ros::spin();

  return 0;
}
