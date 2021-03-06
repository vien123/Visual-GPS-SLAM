/**
* This file is part of DSO.
* 
* Copyright 2016 Technical University of Munich and Intel.
* Developed by Jakob Engel <engelj at in dot tum dot de>,
* for more information see <http://vision.in.tum.de/dso>.
* If you use this code, please cite the respective publications as
* listed on the above website.
*
* DSO is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* DSO is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with DSO. If not, see <http://www.gnu.org/licenses/>.
*/


#pragma once
#include "util/settings.h"
#include "util/globalFuncs.h"
#include "util/globalCalib.h"

#include <sstream>
#include <fstream>
#include <dirent.h>
#include <algorithm>

#include "util/Undistort.h"
#include "IOWrapper/ImageRW.h"

#if HAS_ZIPLIB
	#include "zip.h"
#endif

#include <boost/thread.hpp>

using namespace dso;



inline int getdir (std::string dir, std::vector<std::string> &files)
{
    DIR *dp;
    struct dirent *dirp;
    if((dp  = opendir(dir.c_str())) == NULL)
    {
        return -1;
    }

    while ((dirp = readdir(dp)) != NULL) {
    	std::string name = std::string(dirp->d_name);

    	if(name != "." && name != "..")
    		files.push_back(name);
    }
    closedir(dp);


    std::sort(files.begin(), files.end());

    if(dir.at( dir.length() - 1 ) != '/') dir = dir+"/";
	for(unsigned int i=0;i<files.size();i++)
	{
		if(files[i].at(0) != '/')
			files[i] = dir + files[i];
	}

    return files.size();
}


struct PrepImageItem
{
	int id;
	bool isQueud;
	ImageAndExposure* pt;

	inline PrepImageItem(int _id)
	{
		id=_id;
		isQueud = false;
		pt=0;
	}

	inline void release()
	{
		if(pt!=0) delete pt;
		pt=0;
	}
};




class ImageFolderReader
{
public:
	ImageFolderReader(std::string path, std::string calibFile, std::string gammaFile, std::string vignetteFile, std::string cameraPoses)
	{
		this->path = path;
		this->calibfile = calibFile;
                this->posesfile = cameraPoses; //Added class variable for camera poses

#if HAS_ZIPLIB
		ziparchive=0;
		databuffer=0;
#endif

		isZipped = (path.length()>4 && path.substr(path.length()-4) == ".zip");





		if(isZipped)
		{
#if HAS_ZIPLIB
			int ziperror=0;
			ziparchive = zip_open(path.c_str(),  ZIP_RDONLY, &ziperror);
			if(ziperror!=0)
			{
				printf("ERROR %d reading archive %s!\n", ziperror, path.c_str());
				exit(1);
			}

			files.clear();
			int numEntries = zip_get_num_entries(ziparchive, 0);
			for(int k=0;k<numEntries;k++)
			{
				const char* name = zip_get_name(ziparchive, k,  ZIP_FL_ENC_STRICT);
				std::string nstr = std::string(name);
				if(nstr == "." || nstr == "..") continue;
				files.push_back(name);
			}

			printf("got %d entries and %d files!\n", numEntries, (int)files.size());
			std::sort(files.begin(), files.end());
#else
			printf("ERROR: cannot read .zip archive, as compile without ziplib!\n");
			exit(1);
#endif
		}
		else
			getdir (path, files);


		undistort = Undistort::getUndistorterForFile(calibFile, gammaFile, vignetteFile);


		widthOrg = undistort->getOriginalSize()[0];
		heightOrg = undistort->getOriginalSize()[1];
		width=undistort->getSize()[0];
		height=undistort->getSize()[1];


		// load timestamps if possible.
		loadTimestamps();
		loadCameraPoses();
		printf("ImageFolderReader: got %d files in %s!\n", (int)files.size(), path.c_str());

	}
	~ImageFolderReader()
	{
#if HAS_ZIPLIB
		if(ziparchive!=0) zip_close(ziparchive);
		if(databuffer!=0) delete databuffer;
#endif


		delete undistort;
	};

	Eigen::VectorXf getOriginalCalib()
	{
		return undistort->getOriginalParameter().cast<float>();
	}
	Eigen::Vector2i getOriginalDimensions()
	{
		return  undistort->getOriginalSize();
	}

	void getCalibMono(Eigen::Matrix3f &K, int &w, int &h)
	{
		K = undistort->getK().cast<float>();
		w = undistort->getSize()[0];
		h = undistort->getSize()[1];
	}

	void setGlobalCalibration()
	{
		int w_out, h_out;
		Eigen::Matrix3f K;
		getCalibMono(K, w_out, h_out);
		setGlobalCalib(w_out, h_out, K);
	}

	int getNumImages()
	{
		return files.size();
	}

	double getTimestamp(int id)
	{
		if(timestamps.size()==0) return id*0.1f;
		if(id >= (int)timestamps.size()) return 0;
		if(id < 0) return 0;
		return timestamps[id];
	}


	void prepImage(int id, bool as8U=false)
	{

	}


	MinimalImageB* getImageRaw(int id)
	{
			return getImageRaw_internal(id,0);
	}

	ImageAndExposure* getImage(int id, bool forceLoadDirectly=false)
	{
		return getImage_internal(id, 0);
	}


	inline float* getPhotometricGamma()
	{
		if(undistort==0 || undistort->photometricUndist==0) return 0;
		return undistort->photometricUndist->getG();
	}
        
        inline std::vector<SE3> getCameraPoses()
        {
            return this->cameraPoses;
        }


	// undistorter. [0] always exists, [1-2] only when MT is enabled.
	Undistort* undistort;
private:


	MinimalImageB* getImageRaw_internal(int id, int unused)
	{
		if(!isZipped)
		{
			// CHANGE FOR ZIP FILE
			return IOWrap::readImageBW_8U(files[id]);
		}
		else
		{
#if HAS_ZIPLIB
			if(databuffer==0) databuffer = new char[widthOrg*heightOrg*6+10000];
			zip_file_t* fle = zip_fopen(ziparchive, files[id].c_str(), 0);
			long readbytes = zip_fread(fle, databuffer, (long)widthOrg*heightOrg*6+10000);

			if(readbytes > (long)widthOrg*heightOrg*6)
			{
				printf("read %ld/%ld bytes for file %s. increase buffer!!\n", readbytes,(long)widthOrg*heightOrg*6+10000, files[id].c_str());
				delete[] databuffer;
				databuffer = new char[(long)widthOrg*heightOrg*30];
				fle = zip_fopen(ziparchive, files[id].c_str(), 0);
				readbytes = zip_fread(fle, databuffer, (long)widthOrg*heightOrg*30+10000);

				if(readbytes > (long)widthOrg*heightOrg*30)
				{
					printf("buffer still to small (read %ld/%ld). abort.\n", readbytes,(long)widthOrg*heightOrg*30+10000);
					exit(1);
				}
			}

			return IOWrap::readStreamBW_8U(databuffer, readbytes);
#else
			printf("ERROR: cannot read .zip archive, as compile without ziplib!\n");
			exit(1);
#endif
		}
	}


	ImageAndExposure* getImage_internal(int id, int unused)
	{
		MinimalImageB* minimg = getImageRaw_internal(id, 0);
		ImageAndExposure* ret2 = undistort->undistort<unsigned char>(
				minimg,
				(exposures.size() == 0 ? 1.0f : exposures[id]),
				(timestamps.size() == 0 ? 0.0 : timestamps[id]));
		delete minimg;
		return ret2;
	}

	inline void loadTimestamps()
	{
		std::ifstream tr;
		std::string timesFile = path.substr(0,path.find_last_of('/')) + "/times.txt";
		tr.open(timesFile.c_str());
		while(!tr.eof() && tr.good())
		{
			std::string line;
			char buf[1000];
			tr.getline(buf, 1000);

			int id;
			double stamp;
			float exposure = 0;

			if(3 == sscanf(buf, "%d %lf %f", &id, &stamp, &exposure))
			{
				timestamps.push_back(stamp);
				exposures.push_back(exposure);
			}

			else if(2 == sscanf(buf, "%d %lf", &id, &stamp))
			{
				timestamps.push_back(stamp);
				exposures.push_back(exposure);
			}
		}
		tr.close();

		// check if exposures are correct, (possibly skip)
		bool exposuresGood = ((int)exposures.size()==(int)getNumImages()) ;
		for(int i=0;i<(int)exposures.size();i++)
		{
			if(exposures[i] == 0)
			{
				// fix!
				float sum=0,num=0;
				if(i>0 && exposures[i-1] > 0) {sum += exposures[i-1]; num++;}
				if(i+1<(int)exposures.size() && exposures[i+1] > 0) {sum += exposures[i+1]; num++;}

				if(num>0)
					exposures[i] = sum/num;
			}

			if(exposures[i] == 0) exposuresGood=false;
		}


		if((int)getNumImages() != (int)timestamps.size())
		{
			printf("set timestamps and exposures to zero!\n");
			exposures.clear();
			timestamps.clear();
		}

		if((int)getNumImages() != (int)exposures.size() || !exposuresGood)
		{
			printf("set EXPOSURES to zero!\n");
			exposures.clear();
		}

		printf("got %d images and %d timestamps and %d exposures.!\n", (int)getNumImages(), (int)timestamps.size(), (int)exposures.size());
	}

        /*
         Added function to read in cameraPoses:
         */
	inline void loadCameraPoses()
	{
		std::ifstream poseReader;
		poseReader.open(this->posesfile.c_str());
                float fps = 1.0f / 25.0f;
                int index = 0;
		while(!poseReader.eof() && poseReader.good())
		{
			std::string line;
			char buf[1000];
			poseReader.getline(buf, 1000);
                        
                        /* 
                         A line of the poses.csv file (exported from Blender)
                         looks like this, for instance:
                         
                         40,0.000000,0.001338,0.000000,0.707107,0.707107,0.000000,0.000000
                         
                         it contains:
                         
                         > timestamp (ms), 
                         > world-position-x,
                         > world-position-y,
                         > world-position-z,
                         > world-orientation-w,
                         > world-orientation-x, 
                         > world-orientation-y,
                         > world-orientation-z
                         */
                        
			double stamp;
			double loc_x, loc_y, loc_z = 0;
                        double quat_w, quat_x, quat_y, quat_z = 0;
                        
                        // Extract values from current read line:
			if(8 == sscanf(buf, "%lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf", &stamp, &loc_x, &loc_y, &loc_z, &quat_w, &quat_x, &quat_y, &quat_z))
			{
                                // Create translation vector and quaternion from values
                                Eigen::Vector3d translation(loc_x, loc_y, loc_z);
                                Eigen::Quaterniond orientation(quat_w, quat_x, quat_y, quat_z);
                                
                                std::cout << "\nQuaternion read: (" << orientation.w() << ", " << orientation.x() << ", " << orientation.y() << ", " << orientation.z() << ")" << std::endl;
                                std::cout << "Translation read: (" << translation.x() << ", " << translation.y() << ", " << translation.z() << ")" << std::endl;
                                
                                // Create 4x4 (Affine) Matrix for the current camera Pose
                                Eigen::Affine3d blenderWorld2Camera;
                                blenderWorld2Camera.Identity();
                                blenderWorld2Camera.translation() = translation;
                                blenderWorld2Camera.linear() = orientation.toRotationMatrix();
                                
                                // We actually need the transformation from camera to world:
                                Eigen::Affine3d blenderCamera2World = blenderWorld2Camera.inverse();
                                
                                // Matrix converting from Blender space to DSO space:
                                // x = -x
                                // y = -z
                                // z =  y
                                Eigen::Matrix3d linearB2D(3,3);
                                linearB2D << -1,  0,  0,
                                              0,  0, -1,
                                              0,  1,  0;
                                Eigen::Affine3d blender2DSO;
                                blender2DSO.setIdentity();
                                blender2DSO.linear() = linearB2D;
                                
                                // Transform from Blender coordinate system (BCS) to DSO coordinate system (DCS)
                                Eigen::Affine3d finalPose = blender2DSO * blenderCamera2World;
                                
                                // We are finished, we can create the SE3 representation now:
                                // So, convert to Sophus:
                                SE3 cameraPose(finalPose.linear(), finalPose.translation());
                                
                                // Push current pose onto list, so it can be retrieved for every Frame processed
                                cameraPoses.push_back(cameraPose);
                                
                                
                                
                                
                                // Now transform back (Test - TODO: Put in OutputWrapper)
                                
                                // Matrix converting from DSO space to Blender space (will be used later for visualization):
                                // x = -x
                                // y =  z
                                // z = -y
                                Eigen::Matrix3d linearD2B(3,3);
                                linearD2B << -1,  0,  0,
                                              0,  0,  1,
                                              0, -1,  0;
                                Eigen::Matrix4d dso2Blender;
                                dso2Blender.setIdentity();
                                dso2Blender.topLeftCorner(3,3) = linearD2B;
                                
                                // Test the transformation back to Blender:
                                
                                // Eigen::Affine3d dsoPoseWorking(finalPose);
                                Eigen::Matrix4d dsoPose;
                                dsoPose.setIdentity();
                                dsoPose = cameraPose.matrix();
                                
                                Eigen::Matrix4d blenderCam2World = dso2Blender * dsoPose;
                                Eigen::Matrix4d blenderWorld2Cam = blenderCam2World.inverse(); //Inverse = World to Camera
                                
                                std::cout << dsoPose.matrix() << std::endl;
                                std::cout << finalPose.affine() << std::endl;
                                
                                
                                Eigen::Quaterniond q;
                                q = Eigen::Matrix3d(blenderWorld2Cam.topLeftCorner(3,3) );
                                
                                Eigen::Vector3d t;
                                t = blenderWorld2Cam.topRightCorner(3,1);
                                
                                
                                /*
                                
                                SE3 recoveredSE3(recoveredPose.linear(), recoveredPose.translation());
                                
                                Eigen::Affine3d back = dso2Blender * finalPose;
                                back = back.inverse();
                                
                                */
                                
                                
                                std::cout << "==> Recovered: quat(" << q.w() << ", " << q.x() << ", " << q.y() << ", " << q.z() << ")" << std::endl;
                                std::cout << "==> Recovered: trans(" << t.x() << ", " << t.y() << ", " << t.z() << ")" << std::endl << std::endl;
                                
                                
                                /*
                                // Debug output
                                std::cout << "ADDED a new camera pose quat("
                                        << cameraPose.unit_quaternion().w() << ","
                                        << cameraPose.unit_quaternion().x() << ","
                                        << cameraPose.unit_quaternion().y() << ","
                                        << cameraPose.unit_quaternion().z()
                                        << ")\n"
                                        << cameraPose.matrix3x4()
                                        << "\nNumber: " << index << std::endl;                                
                                
                                
                                
                                
                                std::cout << "Recovered pose quat("
                                        << recoveredSE3.unit_quaternion().w() << ","
                                        << recoveredSE3.unit_quaternion().x() << ","
                                        << recoveredSE3.unit_quaternion().y() << ","
                                        << recoveredSE3.unit_quaternion().z()
                                        << ")\n"
                                        << recoveredSE3.matrix3x4()
                                        << "\nNumber: " << index << std::endl << std::endl; 
                                
                                 * 
                                 * 
                                 * 
                                 * 
                                 * 
                                 * 
                                // Now transform back (Test - TODO: Put in OutputWrapper)
                                
                                // Matrix converting from DSO space to Blender space (will be used later for visualization):
                                // x = -x
                                // y =  z
                                // z = -y
                                Eigen::Matrix3d linearD2B(3,3);
                                linearD2B << -1,  0,  0,
                                              0,  0,  1,
                                              0, -1,  0;
                                Eigen::Affine3d dso2Blender;
                                dso2Blender.setIdentity();
                                dso2Blender.linear() = linearD2B;
                                
                                // Test the transformation back to Blender:
                                Eigen::Affine3d dsoPose(cameraPose);
                                Eigen::Affine3d blenderCam2World = dso2Blender * dsoPose;
                                Eigen::Affine3d blenderWorld2Cam = blenderCam2World.inverse();
                                
                                Eigen::Quaterniond q(blenderWorld2Cam.linear());
                                Eigen::Vector3d t(blenderWorld2Cam.translation());
                                
                                std::cout << "==> Recovered: quat(" << q.w() << ", " << q.x() << ", " << q.y() << ", " << q.z() << ")" << std::endl;
                                std::cout << "==> Recovered: trans(" << t.x() << ", " << t.y() << ", " << t.z() << ")" << std::endl;
                                
                                
                                 * 
                                 * 
                                 * 
                                 * 
                                 * 
                                 * 
                                 * 
                                */
                                
			}
                        
                        index+=1;

		}
		poseReader.close();

		// check if exposures are correct, (possibly skip)
		bool exposuresGood = ((int)exposures.size()==(int)getNumImages()) ;
		for(int i=0;i<(int)exposures.size();i++)
		{
			if(exposures[i] == 0)
			{
				// fix!
				float sum=0,num=0;
				if(i>0 && exposures[i-1] > 0) {sum += exposures[i-1]; num++;}
				if(i+1<(int)exposures.size() && exposures[i+1] > 0) {sum += exposures[i+1]; num++;}

				if(num>0)
					exposures[i] = sum/num;
			}

			if(exposures[i] == 0) exposuresGood=false;
		}


		if((int)getNumImages() != (int)timestamps.size())
		{
			printf("set timestamps and exposures to zero!\n");
			exposures.clear();
			timestamps.clear();
		}

		if((int)getNumImages() != (int)exposures.size() || !exposuresGood)
		{
			printf("set EXPOSURES to zero!\n");
			exposures.clear();
		}

		printf("got %d images and %d timestamps and %d exposures.!\n", (int)getNumImages(), (int)timestamps.size(), (int)exposures.size());
	}



	std::vector<ImageAndExposure*> preloadedImages;
	std::vector<std::string> files;
	std::vector<double> timestamps;
	std::vector<float> exposures;
        std::vector<SE3> cameraPoses; // List of cameraPoses (i.e. groundTruth)

	int width, height;
	int widthOrg, heightOrg;

	std::string path;
	std::string calibfile;
        std::string posesfile; // Path to cameraPoses.csv file

	bool isZipped;

#if HAS_ZIPLIB
	zip_t* ziparchive;
	char* databuffer;
#endif
};

