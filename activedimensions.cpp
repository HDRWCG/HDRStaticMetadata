//
//  main.cpp
//  OpenImageIOTest2
//
//  Created by Patrick Cusack on 3/10/16.
//  Copyright (c) 2016 Patrick Cusack. All rights reserved.
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//  The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


#include <stdio.h>
#include <iostream>
#include <OpenImageIO/imageio.h>

#ifdef __APPLE__
#include <Opencv2/core/core.hpp>
// FOR TESTING ONLY
#include <Opencv2/highgui/highgui.hpp>
#include <Opencv2/imgproc/imgproc.hpp>
#else
#include <opencv2/core/core.hpp>
// FOR TESTING ONLY
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#endif

#define FULLBLACK 0
#define FULLMAXCOLOR 65535

#define RANGEBLACK 4096
#define RANGEMAXCOLOR (60160 - 4096)

OIIO_NAMESPACE_USING
using namespace cv;
using namespace std;

std::pair<int,int> getActiveAreaDimensionsForFilePath(const char * filePath){

    ImageInput *in = ImageInput::open (filePath);
    if (!in)
        return std::make_pair(0,0);
    
    const ImageSpec &spec = in->spec();
    int xres = spec.width;
    int yres = spec.height;
    int channels = spec.nchannels;
    std::vector<uint16_t> pixels (xres*yres*channels*sizeof(uint16_t));
    
    in->read_image (TypeDesc::UINT16, &pixels[0]);
    
    Mat mainImage = Mat(yres, xres, CV_16UC3, pixels.data());
    
    int yOffset = 0;//280;   // <--- YOU NEED TO FIND THIS NUMBER
    yres = yres - yOffset - yOffset;
    Mat cvImage = mainImage(Rect(0, yOffset, xres, yres ));
    
    // Get Color Minimum
    ushort trueBlackR = FULLMAXCOLOR;
    ushort trueBlackG = FULLMAXCOLOR;
    ushort trueBlackB = FULLMAXCOLOR;
    
    ushort trueMaxR = FULLBLACK;
    ushort trueMaxG = FULLBLACK;
    ushort trueMaxB = FULLBLACK;
    
    for (int x = 0; x < xres; x++) {
        for (int y = 0; y < yres; y++) {
            
            Vec3w currentPixel = cvImage.at<Vec3w>(y,x);
            if (currentPixel.val[0] < trueBlackB) {trueBlackB = currentPixel.val[0];}
            if (currentPixel.val[1] < trueBlackG) {trueBlackG = currentPixel.val[1];}
            if (currentPixel.val[2] < trueBlackR) {trueBlackR = currentPixel.val[2];}
            
            if (currentPixel.val[0] > trueMaxB) {trueMaxB = currentPixel.val[0];}
            if (currentPixel.val[1] > trueMaxG) {trueMaxG = currentPixel.val[1];}
            if (currentPixel.val[2] > trueMaxR) {trueMaxR = currentPixel.val[2];}
            
        }
        
    }
        
    ushort minColorComponent = FULLMAXCOLOR;
    minColorComponent = trueBlackR > trueBlackG ? trueBlackG : trueBlackR;
    minColorComponent = minColorComponent > trueBlackB ? trueBlackB : minColorComponent;
    
    ushort maxColorComponent = FULLBLACK;
    maxColorComponent = trueMaxR > trueMaxG ? trueMaxG : trueMaxR;
    maxColorComponent = maxColorComponent > trueMaxB ? trueMaxB : maxColorComponent;
    
    
    bool useFull = false;
    if (minColorComponent < 4096) {
        useFull = true;
    }
    
    int rowStart = -1;
    int rowEnd = -1;
    
    for (int i = 0; i < yres; i++) {
        
        double min = 0.0, max = 0.0;
        Mat rowImage = mainImage(Rect(0, i, xres, 1 ));
        minMaxLoc(rowImage, &min, &max);
        
        if (rowEnd == -1 && rowStart != -1  && min == max) {
            rowEnd = i;
        }
        if (min != max && rowStart == -1) {
            rowStart = i;
        }
        
    }

#ifdef __APPLE__
    ImageInput::destroy (in);
#else
    delete in;
#endif
    
    if (rowEnd <= rowStart) {return std::make_pair(-1, -1);}

    return std::make_pair(rowStart, rowEnd - rowStart);

}
