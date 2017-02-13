# HDRStaticMetadata

 HDR GENERATOR TOOL
 In its essence, this tool will calculate the maxFall and maxCLL of a 16-bit TIFF frame using the formula 'PQ10000_f' (defined by Bill Mandel). This application will scan a folder of
 TIFF files and proceed to perform calculations on the files. File results are calculated concurrently according to the number of threads a user specifies. The
 results are logged to a file. The files processed and the time the files were processed are logged as well. OpenImageIO is used to read the files into a 16bit vector.
 OpenCV is used for conveniently accessing pixels as well as croping an image for frame average light level calculations. QtCore and QtConcurrent are used for 
 abstracted file system access and concurrency respectively. The text files generated in this process are then analyzed in a post process tool to calculate 
 maxFall and maxCLL values at 99.9%.


There are a couple of areas where the code warrants review for further optimization. Currently, each pixel is iterated over in a single "for-loop". I have not optimized this with gcc nor have I explored SIMD calculations to see if that could speed up the process. 
