#ifndef __KERNELS_H__
#define __KERNELS_H__

#include "device_launch_parameters.h"
#include "cuda_runtime.h"

#define STB_IMAGE_IMPLEMENTATION
#include "../stb-master/stb_image.h"

#define BLOCK_SIZE 256
#define COLOR 256
#define PYRAMID_LEVEL 6

typedef struct shift_pair {
	shift_pair() {
		x = 0;
		y = 0;
		angle_ind = 0;
	}
	shift_pair(int _x, int _y) {
		x = _x;
		y = _y;
		angle_ind = 0;
	}
	shift_pair(const shift_pair& rhs) {
		x = rhs.x;
		y = rhs.y;
		angle_ind = rhs.angle_ind;
	}

	int x;
	int y;
	int angle_ind;
} shift_pair;

typedef struct arg_struct {
	shift_pair * shiftp;
	cudaStream_t stream;
	int first_ind;
	int second_ind;
	int width;
	int height;
	uint8_t** mtb;
	uint8_t** ebm;
	uint8_t** shifted_mtb;
	uint8_t** shifted_ebm;
} arguments;

__global__ void rotate(uint8_t* dst, uint8_t* src, int sizeX, int sizeY,
		float deg) {
	int i = blockIdx.x * blockDim.x + threadIdx.x; // Kernel definition
	int j = blockIdx.y * blockDim.y + threadIdx.y;
	int xc = sizeX - sizeX / 2;
	int yc = sizeY - sizeY / 2;
	int newx = ((float) i - xc) * cos(deg) - ((float) j - yc) * sin(deg) + xc;
	int newy = ((float) i - xc) * sin(deg) + ((float) j - yc) * cos(deg) + yc;

	int indis1 = j * sizeX + i;
	int indis2 = newy * sizeX + newx;

	indis1 *= 3;
	indis2 *= 3;

	if (newx >= 0 && newx < sizeX && newy >= 0 && newy < sizeY) {
		dst[indis1++] = src[indis2++];
		dst[indis1++] = src[indis2++];
		dst[indis1] = src[indis2];

		//putPixVal(Destination, sizeX, i , j, ImgSrc[y*ImgWidth+x]);
	}
}

// Simple transformation kernel
__global__ void transformKernel(float* output, cudaTextureObject_t texObj,
		int width, int height) {

	// Calculate normalized texture coordinates
	unsigned int x = blockIdx.x * blockDim.x + threadIdx.x;
	unsigned int y = blockIdx.y * blockDim.y + threadIdx.y;

	float u = x / (float) width;
	float v = y / (float) height;

	// Read from texture and write to global memory

	output[y * width + x] = tex2D<float>(texObj, u, v);

}

__global__ void convert2_GrayScale(float* gray, uint8_t *img, int size,
		int width) {

	unsigned int x = blockIdx.x * blockDim.x + threadIdx.x;
	unsigned int y = blockIdx.y * blockDim.y + threadIdx.y;
	int idx = y * width + x;

	if (idx < size) {
		int index = idx * 3;
		gray[idx] = (54 * img[index] + 183 * img[index + 1]
				+ 19 * img[index + 2]) / 256.0f;
	}
}

__global__ void histogram_smem_atomics(const float *input, int *out, int size) {
	__shared__ int smem[BLOCK_SIZE];

	unsigned int tid = threadIdx.x;
	unsigned int i = blockIdx.x * BLOCK_SIZE + tid;
	unsigned int gridSize = BLOCK_SIZE * gridDim.x;

	smem[tid] = 0;
	__syncthreads();

//	uint8_t pixel;
	while (i < size) {

//		pixel = input[i];
		atomicAdd(&smem[(int) input[i]], 1);

		i += gridSize;
	}
	__syncthreads();

	out[blockIdx.x * BLOCK_SIZE + tid] = smem[tid];
}

__global__ void histogram_final_accum(int n, int *out) {
	int tid = threadIdx.x;
	int i = tid;
	int total = 0;

	while (i < n) {
		total += out[i];
		i += BLOCK_SIZE;
	}
	__syncthreads();

	out[tid] = total;

}

__global__ void find_Median(int n, int *hist, int* median) {
	int half_way = n / 2;
	int sum = 0;

	for (int k = 0; k < COLOR; k++) {
		sum += hist[k];
		if (sum > half_way) {
			*median = k;
			return;
		}
	}
}

__global__ void find_Mtb_Ebm(const float *input, int* median, uint8_t *_mtb,
		uint8_t *_ebm, int _height, int _width) {

	unsigned int x = blockIdx.x * blockDim.x + threadIdx.x;
	unsigned int y = blockIdx.y * blockDim.y + threadIdx.y;
	unsigned int idx = y * _width + x;

	if (input[idx] < (*median - 4) || input[idx] > (*median + 4)) {

		_ebm[idx] = 255;
	} else {
		_ebm[idx] = 0;
	}

	if (input[idx] < *median) {

		_mtb[idx] = 0;
	} else {
		_mtb[idx] = 255;
	}
}

__global__ void AND(uint8_t* output, uint8_t* left, uint8_t *right, int width,
		int size) {

	unsigned int x = blockIdx.x * blockDim.x + threadIdx.x;
	unsigned int y = blockIdx.y * blockDim.y + threadIdx.y;

	int index = y * width + x;
	if (index < size) {
		output[index] = left[index] & right[index];
	}
}

__global__ void XOR(uint8_t* output, uint8_t* left, uint8_t *right, int width,
		int size) {

	unsigned int x = blockIdx.x * blockDim.x + threadIdx.x;
	unsigned int y = blockIdx.y * blockDim.y + threadIdx.y;

	int index = y * width + x;
	if (index < size) {
		output[index] = left[index] ^ right[index];
	}
}

__global__ void count_Errors(const uint8_t *input, int *out, int size) {
	__shared__ int count;

	unsigned int tid = threadIdx.x;
	unsigned int i = blockIdx.x * BLOCK_SIZE + tid;
	unsigned int gridSize = BLOCK_SIZE * gridDim.x;

	count = 0;

	while (i < size) {

		if (input[i] == 255) {
			atomicAdd(&count, 1);
		}

		i += gridSize;
	}

	if (tid == 0) {
		atomicAdd(out, count);
	}
}

__global__ void shift_Image(uint8_t* output, uint8_t* input, int width,
		int height, int x_shift, int y_shift, int j_x, int i_y, int j_width,
		int i_height) {

	int j = blockIdx.x * blockDim.x + threadIdx.x + j_x;
	int i = blockIdx.y * blockDim.y + threadIdx.y + i_y;

	unsigned int input_index = i * width + j;

	unsigned int output_index = y_shift * width + x_shift + i * width + j;

	if (i < i_height && j < j_width) {
		output[output_index] = input[input_index];
	}

}

__global__ void RGB_shift_Image(uint8_t* output, uint8_t* input, int width,
		int height, int x_shift, int y_shift, int j_x, int i_y, int j_width,
		int i_height) {

	int j = blockIdx.x * blockDim.x + threadIdx.x + j_x;
	int i = blockIdx.y * blockDim.y + threadIdx.y + i_y;

	unsigned int input_index = i * width + j;
	input_index *= 3;

	unsigned int output_index = y_shift * width + x_shift + i * width + j;
	output_index *= 3;

	if (i < i_height && j < j_width) {
		output[output_index++] = input[input_index++];
		output[output_index++] = input[input_index++];
		output[output_index] = input[input_index];
	}
}

__global__ void finalShift(uint8_t* output, float* input, int width, int height,
		int x_shift, int y_shift, int j_x, int i_y, int j_width, int i_height) {

	int j = blockIdx.x * blockDim.x + threadIdx.x + j_x;
	int i = blockIdx.y * blockDim.y + threadIdx.y + i_y;

	unsigned int input_index = i * width + j;

	unsigned int output_index = y_shift * width + x_shift + i * width + j;

	if (i < i_height && j < j_width) {
		output[output_index] = input[input_index];
	}
}

bool read_Img(char *filename, uint8_t*& img, int* width, int* height,
		int* bpp) {

	img = stbi_load(filename, width, height, bpp, 3);

	if (img) {
		//	std::cout << filename << " Read Successfully\n";
		return true;
	} else {
		//	std::cout << filename << " Reading Failed\n";
		return false;
	}
}

#endif
