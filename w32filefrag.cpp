// w32filefrag.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

//partial code from:
//https://github.com/zjxylc/FileOffsetOnDisk

#include <iostream>
#include <windows.h>
#include <map>
#include <io.h> 
#include <regex>
#define access    _access_s

#define SUPERMAXPATH 4096

typedef struct _FRAG
{
	LARGE_INTEGER vcnstart;
	LARGE_INTEGER vcnstop;
	LARGE_INTEGER lcnstart;
	LARGE_INTEGER lcnstop;
	LONGLONG length;
}*PFRAG, FRAG;

typedef struct _LCN
{
	LONGLONG lcn_offset;
	LONGLONG lcn_length;
}*PLCN, LCN;

typedef struct _VINFO {
	DWORD ClusterSize;
	ULONGLONG Clusters;
	wchar_t Volume[SUPERMAXPATH];
} VINFO;

bool GetVolInfo(wchar_t* pfname, VINFO* vinfo) {
	bool success = false;

	if (GetVolumePathName(pfname, vinfo->Volume, SUPERMAXPATH)) {
		DWORD SectorsPerCluster;
		DWORD BytesPerSector;
		DWORD NumberOfFreeClusters;
		DWORD TotalNumberOfClusters;

		if (GetDiskFreeSpace(vinfo->Volume, &SectorsPerCluster, &BytesPerSector, &NumberOfFreeClusters, &TotalNumberOfClusters)) {
			//vinfo->Clusters = TotalNumberOfClusters;
			vinfo->ClusterSize = SectorsPerCluster * BytesPerSector;

			//very big volumes don't like the 32 bit integer 
			//16TB at 4KB cluster size maxes out TotalNumberOfClusters

			ULARGE_INTEGER TotalNumberOfBytes;
			if (GetDiskFreeSpaceEx(vinfo->Volume, NULL, &TotalNumberOfBytes, NULL)) {
				vinfo->Clusters = (TotalNumberOfBytes.QuadPart / vinfo->ClusterSize);

				success = true;
			}
		}
	}
	return success;
}



void GetFileOffset(wchar_t* pfname, HANDLE handle,VINFO* vinfo, FILE* outf,bool doMerge)
{
	if (NULL == handle)
	{
		std::cout << "handle is null" << std::endl;
		return;
	}

	STARTING_VCN_INPUT_BUFFER  vcn_buffer;
	RETRIEVAL_POINTERS_BUFFER  retrieval_buffer;
	DWORD                      retbytes = 0;
	DWORD                      error = 0;
	LONGLONG                   pre_vcn;
	LONG                       vcn_num = 0;


	LONGLONG				   total =0;
	LONGLONG				   vcnlength = 0;
	LONGLONG				   frags = 0;
	LONGLONG				unmergedfragcount = 0;
	LONGLONG				one = 1;
	std::map<LONGLONG, PFRAG> fragments;

	vcn_buffer.StartingVcn.QuadPart = pre_vcn = 0;

	PFRAG prev = NULL;
	PFRAG frag = NULL;

	// every time its return only one Extent, if there are many ,you need get many times
	do
	{
		BOOL ret = DeviceIoControl(handle,
			FSCTL_GET_RETRIEVAL_POINTERS,
			&vcn_buffer,
			sizeof(STARTING_VCN_INPUT_BUFFER),
			&retrieval_buffer,
			sizeof(RETRIEVAL_POINTERS_BUFFER),
			&retbytes,
			NULL);

		

		error = GetLastError();
		//std::cout << retrieval_buffer.ExtentCount << std::endl;

		switch (error)
		{
			//errors https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--0-499-
		case ERROR_HANDLE_EOF:
			//std::cout << "file record is end of " << std::endl;
			break;
		case ERROR_MORE_DATA:
			//https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-retrieval_pointers_buffer
			//std::cout << "vcn: [" << retrieval_buffer.StartingVcn.QuadPart << ".." << retrieval_buffer.Extents[0].NextVcn.QuadPart << "] ";
			//setting up for next request
			vcn_buffer.StartingVcn = retrieval_buffer.Extents[0].NextVcn;
			//fall through
		case NO_ERROR:
			vcnlength = retrieval_buffer.Extents[0].NextVcn.QuadPart - pre_vcn;

			
			//std::cout << "lcn: [" << retrieval_buffer.Extents[0].Lcn.QuadPart << ".." << retrieval_buffer.Extents[0].Lcn.QuadPart+vcnlength << "] length: " << vcnlength << std::endl;

			LARGE_INTEGER lcnstart = retrieval_buffer.Extents[0].Lcn;
			LARGE_INTEGER vcnstart = retrieval_buffer.StartingVcn;

			if (prev != NULL && doMerge && 
				((prev->lcnstop.QuadPart) + one) == lcnstart.QuadPart && 
				((prev->vcnstop.QuadPart) + one) == vcnstart.QuadPart
				) {
				//merge interval if the end of prev lcnstop & vcnstop are equal.
				//basically when overreporting is done

				prev->lcnstop.QuadPart = retrieval_buffer.Extents[0].Lcn.QuadPart + vcnlength - one;
				prev->vcnstop.QuadPart = retrieval_buffer.Extents[0].NextVcn.QuadPart - one;
				prev->length = prev->length + vcnlength;
				

			}
			else {
				frag = (PFRAG)malloc(sizeof(FRAG));

				//linux filefrag make an interval from start to end (including)
				//thus [0..0] is one block
				//[0..1] is two blocks
				frag->vcnstart = retrieval_buffer.StartingVcn;
				(frag->vcnstop).QuadPart = retrieval_buffer.Extents[0].NextVcn.QuadPart - one;
				frag->lcnstart = lcnstart;
				(frag->lcnstop).QuadPart = retrieval_buffer.Extents[0].Lcn.QuadPart + vcnlength - one;
				(frag->length) = vcnlength;
				fragments[(frag->lcnstart).QuadPart] = frag;

				frags++;
			}
			unmergedfragcount++;
			
			
			//setting it up for next request calculation
			pre_vcn = vcn_buffer.StartingVcn.QuadPart;
			total += vcnlength;

			break;
		default:
			std::cout << "error code is " << error << std::endl;
			break;
		};

		prev = frag;
	} while (ERROR_MORE_DATA == error);

	fprintf(outf,"file: %ws\n", pfname);
	fprintf(outf,"total_clusters: %lld\n", total);
	fprintf(outf, "cluster_size_b: %ld\n", vinfo->ClusterSize);
	fprintf(outf, "total_size_b: %lld\n", total*(vinfo->ClusterSize));
	fprintf(outf, "extent_count: %lld\n", frags);
	if (doMerge) {
		fprintf(outf, "unmerged_extent_count: %lld\n", unmergedfragcount);
	}

	std::map<LONGLONG, PFRAG>::iterator it;
	fprintf(outf, "extents:\n");
	for (it = fragments.begin(); it != fragments.end(); it++)
	{
		fprintf(outf, " - {lcn: [%lld,%lld], vcn: [%lld,%lld]}\n",
			it->first,
			it->second->lcnstop.QuadPart,
			it->second->vcnstart.QuadPart,
			it->second->vcnstop.QuadPart);
		free(it->second);
	}
	//powershell parsing
	//$f = .\w32filefrag.exe "Backup Job 4\forbackup.51D2023-06-12T143218_4E72.vbk"
	//$csz = $f[2] | ? { $_ -match "cluster_size_b: ([0-9]+)"} | % { [int]$matches[1] } 
	//$fsizecheck = $($sum=0;$f | ? { $_ -match "lcn: \[([0-9]+),([0-9]+)\]"} | % { $sum += [int]$matches[2]-[int]$matches[1] };$sum*$csz)

}

void help() {
	std::cout << "" << std::endl;
	std::cout << "W32FILEFRAG" <<std::endl;
	std::cout << "--------------------------------------------------" << std::endl;
	std::cout << "Dumps the file fragment is the volume offset order" << std::endl;
	std::cout << "" << std::endl;
	std::cout << " w32filefrag -o <file.frag> <file_to_scan>" << std::endl;
	std::cout << "" << std::endl;
	std::cout << "  -h (dumps this help and exits)" << std::endl;
	std::cout << "  -m (merge lcn if possible, this might break the vcn calculation)" << std::endl;
	std::cout << "  -o <file.frag> (optional output file, if no output file is supplied, write to stdout)" << std::endl;
	std::cout << "" << std::endl;
	exit(-1);
}
int main(int argc, char* argv[])
{
	if (argc > 1) {
		
		
		FILE* outf = NULL;
		bool closeout = false;
		char* in_file = NULL ;
		bool doMerge = false;

		//parsing arguments
		//-o outfile infile
		//or 
		//infile
		for (int i = 1; i < argc; i++) {
			if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
				help();
			} else if (strcmp(argv[i], "-m") == 0) {
				doMerge = true;
			}
			else if (strcmp(argv[i],"-o") == 0 && (i+1) < argc) {
				char* outfile = argv[i + 1];
				//check if file exists and if so, refuse to use it
				if (access(outfile, 0) != 0) {
					std::cmatch m;
					std::regex rx(".*\\.frag$");
					const char* first = outfile;
					const char* last = first + strlen(outfile);
					if (std::regex_match(first, last, m, rx)) {
						std::cout << "out_file: " << outfile << std::endl;
						errno_t tryopen = fopen_s(&outf, outfile, "w+");

						if (tryopen == 0) {
							closeout = true;
						}
						else {
							std::cout << "ERROR: fopen_s failed with error :" << tryopen << std::endl;
							help();
						}
					}
					else {
						std::cout << "ERROR: " << outfile << " needs to be a .frag file" << std::endl;
						help();
					}
				} else {
					std::cout << "ERROR: " << outfile << " exists, refusing to write to it" << std::endl;
					exit(5);
				}
				i++;
			} else {
				in_file = argv[i];
				std::cout << "parsing_file: " << in_file << std::endl;
			}
		}

		//real work
		if (in_file != NULL) {
			if (outf == NULL) {
				outf = stdout;
				std::cout << "out_file: stdout" << std::endl;
			}

			HANDLE file_handle = NULL;
			

			wchar_t wstr[SUPERMAXPATH];
			size_t outSize = 0;
			mbstowcs_s(&outSize, wstr, in_file, SUPERMAXPATH);

			

			file_handle = CreateFileA(in_file,
				GENERIC_READ,
				0,
				NULL,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				NULL);

			if (INVALID_HANDLE_VALUE == file_handle)
			{
				std::cout << "ERROR: " << "open file failed ,error = " << GetLastError() << std::endl;
				_fcloseall();
				exit(-3);
			}
			else
			{
				VINFO vinfo;
			
				GetVolInfo(wstr, &vinfo);			
				GetFileOffset(wstr,file_handle,&vinfo,outf,doMerge);
			}


			if (file_handle)
				CloseHandle(file_handle);
			if (closeout)
				fclose(outf);
		}
		else {
			std::cout << "could not find ref file" << std::endl;
			_fcloseall();
			help();
		}
	}
	else {
		std::cout << "ERROR: Argc < 1, You need to pass a filepath for analyzing" << std::endl;
		help();
	}

	
}

