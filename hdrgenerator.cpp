//
//  main.cpp
//  HDR GENERATOR TOOL
//
//  Copyright (c) 2016 Patrick Cusack. All rights reserved.
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//  The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


#include <iostream>
#include <QtCore/QCoreApplication>
#include <QtCore/QCommandLineParser>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QFileInfo>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QDir>
#include <QtCore/QDebug>
#include <QtCore/QDateTime>
#include <QtConcurrent/QtConcurrent>

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

#include "activedimensions.h"

OIIO_NAMESPACE_USING
using namespace cv;


/*
 
 HDR GENERATOR TOOL
 In its essence, this tool will calculate the maxFall and maxCLL of a 16-bit TIFF frame using the formula 'PQ10000_f' (defined by Bill Mandel). This application will scan a folder of
 TIFF files and proceed to perform calculations on the files. File results are calculated concurrently according to the number of threads a user specifies. The
 results will be logged to a file as will the files processed and the time the files were processed. OpenImageIO is used to read the files into a 16bit vector.
 OpenCV is used for conveniently accessing pixels as well as cropoing an image for frame average light level calculations. QtCore and QtConcurrent are used for 
 abstracted file system access and concurrency respectively. The text files generated in this process are then analyzed in a post process tool to calculate 
 maxFall and maxCLL values at 99.9%.
 */

double PQ10000_f( double V){
    //  10000 nits
    //  1/gamma-ish, calculate V from Luma
    //  decode L = (max(,0)/(c2-c3*V**(1/m)))**(1/n)
    //  Lw, Lb not used since absolute Luma used for PQ
    //  formula outputs normalized Luma from 0-1
    
    double L = 0.0;
    L = pow(max(pow(V, 1.0/78.84375) - 0.8359375 ,0.0)/(18.8515625 - 18.6875 * pow(V, 1.0/78.84375)),1.0/0.1593017578);
    return L;
}

Mat resizedMat(Mat input, double scale){
    Mat resizedImage;
    resize(input, resizedImage, cv::Size(), scale, scale, CV_INTER_LINEAR);//CV_INTER_LINEAR CV_INTER_CUBIC
    return resizedImage;
}

typedef struct {
    double maxFALL;
    double maxCLL;
} HDRMetaDataResult;

typedef QPair<QString, HDRMetaDataResult> HDRFileResultPair;

#define CANT_OPEN_FILE {-1., -1.}
#define INVALID_ACTIVE_AREA {-2., -2.}

typedef struct {
    int x;      //These are ignored for now
    int y;
    int width;  //These are ignored for now
    int height;
} HDRActiveArea;

typedef struct {
    int y;
    int height;
    int count;
} HDRActiveAreaSetMember;

typedef struct {
    QString filePath;
    bool use2020;
    bool useFull;
    HDRActiveArea activeArea;
} HDRUserData;

HDRMetaDataResult calculateMetadataForPath(const char * path, bool use2020, bool useFull, HDRActiveArea area){
    
    ImageInput *in = ImageInput::open (path);
    if (!in){
        return CANT_OPEN_FILE;
    }
    
    const ImageSpec &spec = in->spec();
    int xres = spec.width;
    int yres = spec.height;
    int channels = spec.nchannels;
    std::vector<uint16_t> pixels (xres*yres*channels*sizeof(uint16_t));
    
    if ((area.height + area.y) > yres) {
        return INVALID_ACTIVE_AREA;
    }
    
    if (area.height == 0) { area.height = yres;}
    if (area.width == 0) {  area.width = xres;}
    
    //printf("opening file for reading...\n");
    in->read_image (TypeDesc::UINT16, &pixels[0]);
    //printf("finished reading...\n");
    
    //printf("creating opencv mat...\n");
    Mat mainImage = Mat(yres, xres, CV_16UC3, pixels.data());
    
    //printf("creating sub mat\n");
    yres = area.height;
    Mat cvImage = mainImage(Rect(0, area.y, xres, yres ));
    
    //    cv::imshow("Histogram", resizedMat(cvImage, 0.2) );
    //    cv::waitKey(0);
    
    float black = 4096.0;
    float range = 60160.0 - black;
    
    if (useFull == true) {
        black = 0;
        range = 65535.0;
    }
    
    //Create lookup table
    float * lookupTable = (float*)calloc(2 << 16, sizeof(float));
    int pixelMax = 2 << 15;
    for (int i = 0; i < pixelMax; i++) {
        float result = PQ10000_f((float)(i - black) / range);
        *(lookupTable+i) = result;
    }
    
    double mean = 0;
    double maxFALL = 0;
    double maxCLL = 0;
    float LMAX = 0.0;
    double L = 0.0;
    
    for (int x = 0; x < xres; x++) {
        for (int y = 0; y < yres; y++) {
            
            Vec3w currentPixel = cvImage.at<Vec3w>(y,x);
            
            float red = *(lookupTable+currentPixel.val[0]);
            float green = *(lookupTable+currentPixel.val[1]);
            float blue = *(lookupTable+currentPixel.val[2]);
            
            LMAX = fmax(red, green);
            LMAX = fmax(LMAX, blue);
            
            if(use2020) {
                L = (0.2627 * red) + (0.6780 * green) + (0.0593 * blue);
            } else {
                // P3D65:
                L = (0.228975 * red) + (0.691739 * green) + (0.0792869 * blue);
            }
            
            mean += L;
            maxFALL += LMAX;
            
            if(LMAX > maxCLL){
                maxCLL = LMAX;
            }
            
            
        }
        
    }
    
    HDRMetaDataResult result = {10000.0 * (maxFALL/(xres*yres)), 10000.0 * maxCLL};
    
#ifdef __APPLE__
    ImageInput::destroy (in);
#else
    delete in;
#endif
    
    free(lookupTable);
    
    return result;
    
}

static HDRFileResultPair calculateMetadataForPathConcurrently(HDRUserData data){
    
    QByteArray array = data.filePath.toLocal8Bit();
    char * path = array.data();
    bool use2020 = data.use2020;
    bool useFull = data.useFull;
    HDRActiveArea activeArea = data.activeArea;
    
    HDRMetaDataResult result = calculateMetadataForPath((const char *)path, use2020, useFull, activeArea);
    return qMakePair(data.filePath, result);
}

static bool sortHDRUserDataFilePath(const HDRFileResultPair v1, const HDRFileResultPair v2){
    int result =  QString::compare(v1.first, v2.first);
    if (result == 1) {
        return false;
    }
    return true;
}

int getRandomNumber(const int Min, const int Max){
    return ((qrand() % ((Max + 1) - Min)) + Min);
}

QString safeAbsolutePath(QString in){
    
    if (in.startsWith("~/")) {
        in.replace(0, 1, QDir::homePath());
    }

    return QDir(in).canonicalPath();
}

QString currentDateString(){
    return QString(QDateTime::currentDateTime().toString("_MMddyy_hhmm"));
}

QStringList getListOfTiffFilesFromPath(QString path){
    QStringList tiffFiles;
    
    QDirIterator it(path, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString next = it.next();
        if(next.endsWith("tiff", Qt::CaseInsensitive) == true ||
            next.endsWith("tif", Qt::CaseInsensitive) == true){
            tiffFiles.append(next);
        }
    }
    
    tiffFiles.sort(Qt::CaseInsensitive);
    return tiffFiles;
}

QStringList getListOfFilesFromFileStream(QString path){
    
    QFile file(path);
    QStringList list;
    if (file.open(QIODevice::ReadOnly)) {

        QTextStream stream(&file);

        while (!stream.atEnd()) {
            QString nextProcessedFile = stream.readLine();
            if(nextProcessedFile == QString("")){
                continue;
            }
            list << nextProcessedFile;
        }

    } else {
        std::cout << "Unable to open the processed file log." << std::endl;
    }

    return list;
}

typedef QMap<QString, QString> FilePathMap;

FilePathMap getMapOfFilesFromFilePath(QString path){
    
    FilePathMap filePathMap;
    
    QFile filePathFile(path);
    if (filePathFile.open(QIODevice::ReadOnly)) {
        
        QTextStream stream(&filePathFile);
        
        while (!stream.atEnd()) {
            QString nextProcessedFile = stream.readLine();
            
            //Check path seperator as QFileInfo doesn't work when parsing Windows paths on Linux
            if (nextProcessedFile.contains("\\")) {
                QString lastPathComponent = nextProcessedFile.split("\\").last();
                if (lastPathComponent != QString("")) {
                    filePathMap[lastPathComponent] = nextProcessedFile;
                }
            } else {
                QString lastPathComponent = nextProcessedFile.split("/").last();
                if (lastPathComponent != QString("")) {
                    filePathMap[lastPathComponent] = nextProcessedFile;
                }
            }
            
        }
        
    } else {
        std::cout << "Unable to open the file:" << path.toLatin1().data() <<  std::endl;
    }
    
    return filePathMap;
    
}

FilePathMap getMapOfFilesFromList(QStringList list){
    
    FilePathMap filePathMap;
    
    QStringListIterator iter(list);
    
    while (iter.hasNext()) {
        
        QString next = iter.next();
        
        if (next.contains("\\")) {
            QString lastPathComponent = next.split("\\").last();
            if (lastPathComponent != QString("")) {
                filePathMap[lastPathComponent] = next;
            }
        } else if (next.contains("/")){
            QString lastPathComponent = next.split("/").last();
            if (lastPathComponent != QString("")) {
                filePathMap[lastPathComponent] = next;
            }
        } else {
            filePathMap[next] = next;
        }
        
    }
    
    return filePathMap;
}

int main(int argc, const char * argv[]) {

    // std::cout << currentDateString().toLatin1().data() << std::endl;
    // QString path = "hdr_log" + currentDateString() + ".txt";
    // std::cout << path.toLatin1().data() << std::endl;
    
    QCoreApplication app(argc, (char**)argv);
    QCoreApplication::setApplicationName("HDR Meta Data Logger");
    QCoreApplication::setApplicationVersion("0.1");
    
    //PARSER OPTIONS
    
    QCommandLineParser parser;
    //app sourcefolder -r FULL LEGAL -c 2020 P3 -l loglist -p processedfiles
    parser.setApplicationDescription("HDR Meta Helper");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("folder", QCoreApplication::translate("main", "Source file to calculate."));
    
    //r, c, y, d, l, m, p, n, t
    
    QCommandLineOption rangeOption(QStringList() << "r" << "range",
                                   QCoreApplication::translate("main", "Select a luminance range (FULL LEGAL) <range>."),
                                   QCoreApplication::translate("main", "range"));
    parser.addOption(rangeOption);
    
    QCommandLineOption colorOption(QStringList() << "c" << "colorspace",
                                   QCoreApplication::translate("main", "Select a color space (2020 P3) <range>."),
                                   QCoreApplication::translate("main", "colorspace"));
    
    parser.addOption(colorOption);
    
    QCommandLineOption yOffsetOption(QStringList() << "y" << "y offset",
                                     QCoreApplication::translate("main", "Specify a y offset."),
                                     QCoreApplication::translate("main", "y offset"));
    
    parser.addOption(yOffsetOption);
    
    QCommandLineOption yLengthOption(QStringList() << "d" << "y length",
                                     QCoreApplication::translate("main", "Specify a y length."),
                                     QCoreApplication::translate("main", "y length"));
    
    parser.addOption(yLengthOption);
    
    QCommandLineOption loglistOption(QStringList() << "l" << "loglist",
                                     QCoreApplication::translate("main", "Specify a filepath to log processed files <logFilePath>."),
                                     QCoreApplication::translate("main", "loglist"));
    parser.addOption(loglistOption);
    
    QCommandLineOption mandatoryFileListOption(QStringList() << "m" << "filelist",
                                               QCoreApplication::translate("main", "Specify a filepath of files to process."),
                                               QCoreApplication::translate("main", "filelist"));
    parser.addOption(mandatoryFileListOption);
    
    QCommandLineOption processedFilesOption(QStringList() << "p" << "processedfiles",
                                            QCoreApplication::translate("main", "Specify a filepath to retrieve processed files <processedFilePath>."),
                                            QCoreApplication::translate("main", "processedfiles"));
    
    parser.addOption(processedFilesOption);
    
    QCommandLineOption resultFilePathOptions(QStringList() << "n" << "resultFile",
                                             QCoreApplication::translate("main", "Specify a filepath to save the results <resultFile>."),
                                             QCoreApplication::translate("main", "resultFile"));
    
    parser.addOption(resultFilePathOptions);
    
    QCommandLineOption threadCountOption(QStringList() << "t" << "threadCount",
                                         QCoreApplication::translate("main", "Specify the number of threads."),
                                         QCoreApplication::translate("main", "threadCount"));
    
    parser.addOption(threadCountOption);
    
    
    //PROCESS APPLICATION
    parser.process(app);
    
    //Range
    bool rangeOptionFlag = parser.isSet(rangeOption);
    QString rangeOptionString = parser.value(rangeOption);
    
    QStringList validRangeOptions;
    validRangeOptions << QString("") << QString("FULL") << QString("LEGAL");
    if (validRangeOptions.indexOf(rangeOptionString) == -1) {
        std::cout << "Invalid parameter passed to range option: (FULL LEGAL)" << "Found" << rangeOptionString.toLatin1().data() << "instead" << std::endl;
        return -1;
    }
    
    bool useFull = true;
    
    if (rangeOptionFlag  == true && rangeOptionString == QString("LEGAL")) {
        useFull = false;
    }
    
    //Color
    //check your arguments and set defaults, always use 2020 and full range unless otherwise specified
    bool colorOptionFlag = parser.isSet(colorOption);
    QString colorOptionString = parser.value(colorOption);
    
    QStringList validColorOptions;
    validColorOptions << QString("") << QString("2020") << QString("P3");
    if (validColorOptions.indexOf(colorOptionString) == -1) {
        std::cout << "Invalid parameter passed to color option: (2020 P3)" << "Found" << rangeOptionString.toLatin1().data() << "instead" << std::endl;
        return -1;
    }
    
    bool use2020 = true;
    
    if (colorOptionFlag  == true && colorOptionString == QString("P3")) {
        use2020 = false;
    }
    
    int yOffset = 0;
    int yLength = 0;

    if (parser.isSet(yOffsetOption) == true){yOffset = atoi(parser.value(yOffsetOption).toLatin1().data());}    //yOffset
    if (parser.isSet(yLengthOption) == true){yLength = atoi(parser.value(yLengthOption).toLatin1().data());}    //yLength

    //LIFTED START

    //Log list
    //Check log list path, does not need to exist
    bool loglistFileFlag = parser.isSet(loglistOption);
    QString loglistFilePath = parser.value(loglistOption);
    
    if (loglistFileFlag == true) {
        loglistFilePath = safeAbsolutePath(loglistFilePath);
    } else {
        QString path = "hdr_log" + currentDateString() + ".txt";
        loglistFilePath = QDir::currentPath();
        loglistFilePath = QDir(loglistFilePath).filePath(path.toLatin1().data());
    }
    
    //file list check
    //Check file list path to check
    bool mandatoryFileListFilesFlag = parser.isSet(mandatoryFileListOption);
    QString mandatoryFileListFilesFilePath = parser.value(mandatoryFileListOption);
    
    if (mandatoryFileListFilesFlag == true) {
        mandatoryFileListFilesFilePath = safeAbsolutePath(mandatoryFileListFilesFilePath);
    }
    
    //Processed File
    //Check processed list path, MUST exist
    bool processedFilesFlag = parser.isSet(processedFilesOption);
    QString processedFilesFilePath = parser.value(processedFilesOption);
    
    if (processedFilesFlag == true) {
        processedFilesFilePath = safeAbsolutePath(processedFilesFilePath);
    } else {
        QString path = "hdr_log" + currentDateString() + ".txt";
        processedFilesFilePath = QDir::currentPath();
        processedFilesFilePath = QDir(processedFilesFilePath).filePath(path.toLatin1().data());
    }
    
    //Result File Log Path
    //Check that the user has passed in a folder to scan and that the path is valid
    bool resultFilePathFlag = parser.isSet(resultFilePathOptions);
    QString resultFilePath = parser.value(resultFilePathOptions);
    
    if (resultFilePathFlag == true) {
        resultFilePath = safeAbsolutePath(resultFilePath);
    } else {
        QString path = "hdr_results" + currentDateString() + ".txt";
        resultFilePath = QDir::currentPath();
        resultFilePath = QDir(resultFilePath).filePath(path.toLatin1().data());
    }

    //Check that the user has passed in a folder to scan and that the path is valid
    const QStringList args = parser.positionalArguments();
    QString scanPath = args.count() > 0 ? args.at(0): QDir::currentPath();
    
    scanPath = safeAbsolutePath(scanPath);
    
    QFileInfo pathInfo = QFileInfo(scanPath);
    
    if (!pathInfo.exists()) {
        std::cout << scanPath.toLatin1().data() << "does not exist." << std::endl;
        return -1;
    }
    
    if (!pathInfo.isDir()) {
        std::cout << scanPath.toLatin1().data() << "is not a folder path." << std::endl;
        return -1;
    }
    
    std::cout << "Starting!" << std::endl;
    std::cout << "Scanning Files... " << std::endl;

    //Scan the directory and collect all of the files to process
    QStringList foundTiffFiles = getListOfTiffFilesFromPath(scanPath);
    
    //We need to make sure that the mandatory files exist in either
    if (mandatoryFileListFilesFlag == true) {
        
        QStringList mandatorytFilesList = getListOfFilesFromFileStream(mandatoryFileListFilesFilePath);
        if (mandatorytFilesList.count() == 0) {
            std::cout << "Unable to open the processed file log OR the file was empty." << std::endl;
            return -1;
        }
        
        FilePathMap mandatoryFilesMap = getMapOfFilesFromList(mandatorytFilesList);
        FilePathMap foundTiffFilesMap = getMapOfFilesFromList(foundTiffFiles);
        QStringList finalPrunedFileList;
        int countOfMissingFiles = 0;
        
        QStringListIterator iter(mandatoryFilesMap.keys());
        
        while (iter.hasNext()) {
            QString next = iter.next();
            
            if(next == QString("")){
                continue;
            }
            
            if (!foundTiffFilesMap.contains(next)) {
                qDebug() << next;
                std::cout << "Can't find the following file in the list:" << next.toLatin1().data() << std::endl;
                countOfMissingFiles++;
            } else {
                finalPrunedFileList << foundTiffFilesMap[next];
            }
        }
        
        if (countOfMissingFiles > 0) {
            std::cout << countOfMissingFiles << " FILE(S) ARE MISSING! DO YOU WANT TO CONTINUE? (Y)es or (N)o?" << std::endl;
            std::string result;
            std::getline(std::cin, result);
            if (result == "N" || result == "n") {
                std::cout << "Aborting!!!" << std::endl;
                return -1;
            } else {
                std::cout << "Continuing" << std::endl;
            }
        }
        
        
        foundTiffFiles = finalPrunedFileList;
        
    }
    
    //Get all of the processed files so far if any and remove them from the list of found files
    if (processedFilesFlag == true) {
        QStringList processedFilesList = getListOfFilesFromFileStream(processedFilesFilePath);
        if (processedFilesList.count() == 0) {
            std::cout << "Unable to open the processed file log OR the file was empty." << std::endl;
            return -1;
        }
        
        FilePathMap processedFilesMap = getMapOfFilesFromList(processedFilesList);
        FilePathMap foundTiffFilesMap = getMapOfFilesFromList(foundTiffFiles);
        QStringList foundFilesMinusProcessed;
        
        QStringListIterator iter(foundTiffFilesMap.keys());
        
        while (iter.hasNext()) {
            QString next = iter.next();
            if (!processedFilesMap.contains(next)) {
                foundFilesMinusProcessed << foundTiffFilesMap[next];
            }
        }
        
        foundTiffFiles = foundFilesMinusProcessed;
    }
    
    /***** Create a map of active areas from a sampling of the files *****/
    /***** Check this if the user hasn't specifically specified a y offset or length amount *****/

    if(parser.isSet(yOffsetOption) == false && parser.isSet(yLengthOption) == false){
        
        std::cout << "Scanning Active Dimensions... " << std::endl;
        
        QMap<QString, HDRActiveAreaSetMember> activeAreaResultMap;
        int numberOfFilesToCheck = 10;
        int countOfFilesToCheck = foundTiffFiles.size() > numberOfFilesToCheck ? numberOfFilesToCheck : foundTiffFiles.size();
        for (int i = 0; i < countOfFilesToCheck; i++){
            int index = getRandomNumber(0,foundTiffFiles.size() - 1);
            std::pair<int, int> result = getActiveAreaDimensionsForFilePath(foundTiffFiles.at(index).toLatin1().data());
            
            std::stringstream resultString;
            resultString << result.first << "," << result.second;
            QString qResultString = QString::fromStdString(resultString.str());
            
            if (activeAreaResultMap.contains(qResultString)) {
                HDRActiveAreaSetMember current = activeAreaResultMap[qResultString];
                current.count++;
                activeAreaResultMap[qResultString] = current;
            } else {
                activeAreaResultMap[qResultString] = {result.first, result.second, 1};
            }
        
        }
        
    
        QMapIterator<QString, HDRActiveAreaSetMember> i(activeAreaResultMap);
        QString highestKey;
        int highestCount = 0;
        HDRActiveAreaSetMember highestMember;
        
        while (i.hasNext()) {
            i.next();
            HDRActiveAreaSetMember current = i.value();
            if (current.count > highestCount ) {
                highestCount = current.count;
                highestKey = i.key();
                highestMember = current;
            }
        }
        
        std::cout << "Dimensions with the highest count " << highestKey.toLatin1().data() << ": " << highestCount << " of " << countOfFilesToCheck << " checked" << std::endl;
        
        if (activeAreaResultMap.count() > 0) {
            std::cout << "!!!!! NOT ALL OF THE FILES HAVE THE SAME ACTIVE DIMENSION AREA!!!!!" << std::endl;
            std::cout << "THE FOLLOWING DIMENSION COUNTS WERE FOUND!!!" << std::endl;
            
            QMapIterator<QString, HDRActiveAreaSetMember> i(activeAreaResultMap);
            while (i.hasNext()) {
                i.next();
                HDRActiveAreaSetMember current = i.value();
               std::cout << i.key().toLatin1().data() << ": " << current.count << std::endl;
            }
            
            std::cout << "DO YOU WANT TO CONTINUE AND USE THE HIGHEST COUNT? OTHERWISE PRESS N AND RESTART SPECIFYING THE Y AND LENGTH PARAMETERS FROM THE COMMAND LINE." << std::endl;
            
            std::string result;
            std::getline(std::cin, result);
            if (result == "N" || result == "n") {
                std::cout << "Aborting!!!" << std::endl;
                return -1;
            } else {
                std::cout << "Continuing" << std::endl;
            }
        }
        
        yOffset = highestMember.y;
        yLength = highestMember.height;
        
    }

    //LIFTED END

    //This is a sanity check
    if (yLength == 0) {
        std::cout << "You must specify a vertical pixel length greater than 0, i.e. -d 1600." << std::endl;
        return -1;
    }
    
    std::cout << "Will begin processing the path " << scanPath.toLatin1().data() << ":" << std::endl;
    std::cout  << "The following parameters:" << std::endl;
    
    int numberOfThreads = 4;
    
    if (parser.isSet(threadCountOption)) {
        numberOfThreads = atoi(parser.value(threadCountOption).toLatin1().data());
    }
    
    if (numberOfThreads == 0) {
        std::cout << "You must specify a y length greater than 0." << std::endl;
        //qDebug() << "You must specify a number of threads greater than 0.";
        return -1;
    }
    
    if (useFull == true) {
        std::cout << "\t" << "Use Full Range" << std::endl;
    } else {
        std::cout << "\t" << "Use Legal Range" << std::endl;
    }
    
    if (use2020 == true) {
        std::cout << "\t" << "Use 2020 Color Space" << std::endl;
    } else {
        std::cout << "\t" << "Use P3 Color Space" << std::endl;
    }
    
    std::cout << "\t" << "yOffset" << " " << yOffset << std::endl;
    std::cout << "\t" << "y length" << " " << yLength  << std::endl;
    std::cout << "\t" << "loglistFilePath" << " " << loglistFilePath.toLatin1().data()  << std::endl;
    std::cout << "\t" << "processedFilesFilePath" << " " << processedFilesFilePath.toLatin1().data()  << std::endl;
    std::cout << "\t" << "resultFilePath" << " " << resultFilePath.toLatin1().data() << std::endl;
    std::cout << "\t" << "numberOfThreads" << " " << numberOfThreads << std::endl;
    
    //LIFTED
    
    //OK, now ready to process files
    
    std::cout << "Ready to process: " << foundTiffFiles.size() << " files." << std::endl;
    
    //Create log file
    QFile fileLogFile(loglistFilePath);
    if (!fileLogFile.open(QIODevice::Append | QIODevice::Text)) {
        return -1;
    }
    
    QTextStream logFileStream(&fileLogFile);
    QDateTime current = QDateTime::currentDateTime();
    logFileStream << current.toString() << "\n";
    
    //Create result file
    QFile resultLogFile(resultFilePath);
    if (!resultLogFile.open(QIODevice::Append | QIODevice::Text)) {
        std::cout << "Can't open result file path" << std::endl;
        return -1;
    }
    
    QTextStream resultFileStream(&resultLogFile);
    
    //define active area
    HDRActiveArea area = {0, yOffset, 0, yLength};
    
    bool singleThreaded = false;
    
    if (singleThreaded || foundTiffFiles.size() < numberOfThreads) {
        
        for (int i = 0; i < foundTiffFiles.size(); i++) {
            QString nextTiffFile = foundTiffFiles.at(i);
            HDRMetaDataResult result = calculateMetadataForPath((const char *)nextTiffFile.toLatin1().data(), use2020, useFull, area);
            resultFileStream << nextTiffFile << "\t" << result.maxFALL << "\t"  << result.maxCLL << "\n";
            logFileStream << nextTiffFile << "\t" << QDateTime::currentDateTime().toString().toLatin1().data() << "\n";
        }
        
    } else {
        
        int mod = foundTiffFiles.size() % numberOfThreads;
        int max = foundTiffFiles.size();
        int maxNormal = foundTiffFiles.size() - mod;
        
        for (int i = 0; i < maxNormal; i += numberOfThreads) {
            
            QList<HDRUserData> userDataList;
            
            for (int y = 0; y < numberOfThreads; y++) {
                HDRUserData data = {foundTiffFiles.at(i + y), use2020, useFull, area};
                userDataList << data;
            }
            
            QList<HDRFileResultPair> results = QtConcurrent::blockingMapped(userDataList, calculateMetadataForPathConcurrently);
            qSort(results.begin(), results.end(), sortHDRUserDataFilePath);
            
            QListIterator<HDRFileResultPair> iter(results);
            while (iter.hasNext()) {
                HDRFileResultPair result = iter.next();
                resultFileStream << result.first << "\t" << result.second.maxFALL << "\t"  << result.second.maxCLL << "\n";
                logFileStream << result.first << "\t" << QDateTime::currentDateTime().toString().toLatin1().data() << "\n";
            }
            
        }
        
        for (int i = maxNormal; i < max; i++) {
            QString nextTiffFile = foundTiffFiles.at(i);
            HDRMetaDataResult result = calculateMetadataForPath((const char *)nextTiffFile.toLatin1().data(), use2020, useFull, area);
            resultFileStream << nextTiffFile << "\t" << result.maxFALL << "\t"  << result.maxCLL << "\n";
            logFileStream << nextTiffFile << "\t" << QDateTime::currentDateTime().toString().toLatin1().data() << "\n";
        }
        
        
    }
    
    std::cout << "Finished!" << std::endl;
    
    return 0;
}
