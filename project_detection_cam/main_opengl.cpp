/*** Include ***/
/* for general */
#include <stdint.h>
#include <stdio.h>
#include <fstream> 
#include <vector>
#include <string>
#include <chrono>

/* for OpenCV */
#include <opencv2/opencv.hpp>

/* For OpenGL */
#include <GL/glut.h>
#include <GL/freeglut.h>

/* for Tensorflow Lite */
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/model.h"
#include "tensorflow/lite/kernels/register.h"

/* for Edge TPU */
#ifdef USE_EDGETPU
#include "edgetpu.h"
#include "edgetpu_c.h"
#endif

/*** Macro ***/
/* Model parameters */
#ifdef USE_EDGETPU
#define USE_EDGETPU_DELEGATE
#define MODEL_FILENAME RESOURCE"/coco_ssd_mobilenet_v1_1.0_quant_2018_06_29_edgetpu.tflite"
#else
#define MODEL_FILENAME RESOURCE"/coco_ssd_mobilenet_v1_1.0_quant_2018_06_29.tflite"
#endif
#define LABEL_NAME     RESOURCE"/coco_ssd_mobilenet_v1_1.0_quant_2018_06_29.txt"

/* Settings */

#define TFLITE_MINIMAL_CHECK(x)                              \
  if (!(x)) {                                                \
    fprintf(stderr, "Error at %s:%d\n", __FILE__, __LINE__); \
    exit(1);                                                 \
  }

typedef struct {
	double x;
	double y;
	double w;
	double h;
	int classId;
	std::string classIdName;
	double score;
} BBox;

static cv::VideoCapture cap;
static std::vector<std::string> labels;
static std::unique_ptr<tflite::Interpreter> interpreter;

/*** Function ***/
static void displayModelInfo(const tflite::Interpreter* interpreter)
{
	const auto& inputIndices = interpreter->inputs();
	int inputNum = (int)inputIndices.size();
	printf("Input num = %d\n", inputNum);
	for (int i = 0; i < inputNum; i++) {
		auto* tensor = interpreter->tensor(inputIndices[i]);
		for (int j = 0; j < tensor->dims->size; j++) {
			printf("    tensor[%d]->dims->size[%d]: %d\n", i, j, tensor->dims->data[j]);
		}
		if (tensor->type == kTfLiteUInt8) {
			printf("    tensor[%d]->type: quantized\n", i);
			printf("    tensor[%d]->params.outputZeroPoint, scale: %d, %f\n", i, tensor->params.zero_point, tensor->params.scale);
		} else {
			printf("    tensor[%d]->type: not quantized\n", i);
		}
	}

	const auto& outputIndices = interpreter->outputs();
	int outputNum = (int)outputIndices.size();
	printf("Output num = %d\n", outputNum);
	for (int i = 0; i < outputNum; i++) {
		auto* tensor = interpreter->tensor(outputIndices[i]);
		for (int j = 0; j < tensor->dims->size; j++) {
			printf("    tensor[%d]->dims->size[%d]: %d\n", i, j, tensor->dims->data[j]);
		}
		if (tensor->type == kTfLiteUInt8) {
			printf("    tensor[%d]->type: quantized\n", i);
			printf("    tensor[%d]->params.outputZeroPoint, scale: %d, %f\n", i, tensor->params.zero_point, tensor->params.scale);
		} else {
			printf("    tensor[%d]->type: not quantized\n", i);
		}
	}
}

static void extractTensorAsFloatVector(tflite::Interpreter *interpreter, const int index, std::vector<float> &output)
{
	const TfLiteTensor* tensor = interpreter->tensor(index);
	int dataNum = 1;
	for (int i = 0; i < tensor->dims->size; i++) {
		dataNum *= tensor->dims->data[i];
	}
	output.resize(dataNum);
	if (tensor->type == kTfLiteUInt8) {
		const auto *valUint8 = interpreter->typed_tensor<uint8_t>(index);
		for (int i = 0; i < dataNum; i++) {
			float valFloat = (valUint8[i] - tensor->params.zero_point) * tensor->params.scale;
			output[i] = valFloat;
		}
	} else {
		const auto *valFloat = interpreter->typed_tensor<float>(index);
		for (int i = 0; i < dataNum; i++) {
			output[i] = valFloat[i];
		}
	}
}

static TfLiteFloatArray* TfLiteFloatArrayCopy(const TfLiteFloatArray* src) {
	if (!src) return nullptr;
	TfLiteFloatArray* ret = static_cast<TfLiteFloatArray*>(
		malloc(TfLiteFloatArrayGetSizeInBytes(src->size)));
	if (!ret) return nullptr;
	ret->size = src->size;
	std::memcpy(ret->data, src->data, src->size * sizeof(float));
	return ret;
}

static void setBufferToTensor(tflite::Interpreter *interpreter, const int index, const char *data, const int dataSize)
{
	const TfLiteTensor* inputTensor = interpreter->tensor(index);
	const int modelInputHeight = inputTensor->dims->data[1];
	const int modelInputWidth = inputTensor->dims->data[2];
	const int modelInputChannel = inputTensor->dims->data[3];

	if (inputTensor->type == kTfLiteUInt8) {
		TFLITE_MINIMAL_CHECK(sizeof(int8_t) * 1 * modelInputHeight * modelInputWidth * modelInputChannel == dataSize);
		/* Need deep copy quantization parameters */
		/* reference: https://github.com/google-coral/edgetpu/blob/master/src/cpp/basic/basic_engine_native.cc */
		/* todo: release them */
		const TfLiteAffineQuantization* inputQuantParams = reinterpret_cast<TfLiteAffineQuantization*>(inputTensor->quantization.params);
		TfLiteQuantization inputQuantClone;
		inputQuantClone = inputTensor->quantization;
		TfLiteAffineQuantization* inputQuantParamsClone = reinterpret_cast<TfLiteAffineQuantization*>(malloc(sizeof(TfLiteAffineQuantization)));
		inputQuantParamsClone->scale = TfLiteFloatArrayCopy(inputQuantParams->scale);
		inputQuantParamsClone->zero_point = TfLiteIntArrayCopy(inputQuantParams->zero_point);
		inputQuantParamsClone->quantized_dimension = inputQuantParams->quantized_dimension;
		inputQuantClone.params = inputQuantParamsClone;

		//memcpy(inputTensor->data.int8, data, sizeof(int8_t) * 1 * modelInputWidth * modelInputHeight * modelInputChannel);
		interpreter->SetTensorParametersReadOnly(
			index, inputTensor->type, inputTensor->name,
			std::vector<int>(inputTensor->dims->data, inputTensor->dims->data + inputTensor->dims->size),
			inputQuantClone,	// use copied parameters
			data, dataSize);
	} else {
		TFLITE_MINIMAL_CHECK(sizeof(float) * 1 * modelInputHeight * modelInputWidth * modelInputChannel == dataSize);
		//memcpy(inputTensor->data.f, data, sizeof(float) * 1 * modelInputWidth * modelInputHeight * modelInputChannel);
		interpreter->SetTensorParametersReadOnly(
			index, inputTensor->type, inputTensor->name,
			std::vector<int>(inputTensor->dims->data, inputTensor->dims->data + inputTensor->dims->size),
			inputTensor->quantization,
			data, sizeof(float) * 1 * modelInputWidth * modelInputHeight * modelInputChannel);
	}
}

static void readLabel(const char* filename, std::vector<std::string> & labels)
{
	std::ifstream ifs(filename);
	if (ifs.fail()) {
		printf("failed to read %s\n", filename);
		return;
	}
	std::string str;
	while (getline(ifs, str)) {
		labels.push_back(str);
	}
}

static void getBBox(std::vector<BBox> &bboxList, const float *outputBoxList, const float *outputClassList, const float *outputScoreList, const int outputNum, const double threshold, const int imageWidth = 0, const int imageHeight = 0)
{
	for (int i = 0; i < outputNum; i++) {
		int classId = (int)outputClassList[i] + 1;
		float score = outputScoreList[i];
		if (score < threshold) continue;
		float y0 = outputBoxList[4 * i + 0];
		float x0 = outputBoxList[4 * i + 1];
		float y1 = outputBoxList[4 * i + 2];
		float x1 = outputBoxList[4 * i + 3];
		float w = x1 - x0 + 1;
		float h = y1 - y0 + 1;
		if (imageWidth != 0) {
			x0 *= imageWidth;
			x1 *= imageWidth;
			y0 *= imageHeight;
			y1 *= imageHeight;
		}
		//printf("%d[%.2f]: %.3f %.3f %.3f %.3f\n", classId, score, x0, y0, x1, y1);
		BBox bbox;
		bbox.x = x0;
		bbox.y = y0;
		bbox.w = x1 - x0;
		bbox.h = y1 - y0;
		bbox.classId = classId;
		bbox.score = score;
		bboxList.push_back(bbox);
	}
}

static void display(void)
{
	const auto& timeAll0 = std::chrono::steady_clock::now();
	/* Read input image data */
	const auto& timeCap0 = std::chrono::steady_clock::now();
	cv::Mat originalImage;
	cap.read(originalImage);
	const auto& timeCap1 = std::chrono::steady_clock::now();
	static cv::Mat inputImage;	// need to exist longer than interpreter (todo. can be better code...)

	/* Pre-process and Set data to input tensor */
	const auto& timePre0 = std::chrono::steady_clock::now();
	const TfLiteTensor* inputTensor = interpreter->input_tensor(0);
	const int modelInputHeight = inputTensor->dims->data[1];
	const int modelInputWidth = inputTensor->dims->data[2];
	cv::cvtColor(originalImage, originalImage, cv::COLOR_BGR2RGB);
	cv::resize(originalImage, inputImage, cv::Size(modelInputWidth, modelInputHeight));
	if (inputTensor->type == kTfLiteUInt8) {
		//inputImage.convertTo(inputImage, CV_8UC3);
	} else {
		inputImage.convertTo(inputImage, CV_32FC3, 1.0 / 255);
	}

	static int s_setInputBufferOnce = 0;	// call this only once (todo. can be better code...)
	if (s_setInputBufferOnce++ == 0) {
		setBufferToTensor(interpreter.get(), interpreter->inputs()[0], (char*)inputImage.data, (int)(inputImage.total() * inputImage.elemSize()));
	}
	const auto& timePre1 = std::chrono::steady_clock::now();

	/* Run inference */
	const auto& timeInference0 = std::chrono::steady_clock::now();
	TFLITE_MINIMAL_CHECK(interpreter->Invoke() == kTfLiteOk);
	const auto& timeInference1 = std::chrono::steady_clock::now();

	/* Retrieve the result */
	const auto& timePost0 = std::chrono::steady_clock::now();
	std::vector<float> outputBoxList;
	std::vector<float> outputClassList;
	std::vector<float> outputScoreList;
	std::vector<float> outputNumList;
	extractTensorAsFloatVector(interpreter.get(), interpreter->outputs()[0], outputBoxList);
	extractTensorAsFloatVector(interpreter.get(), interpreter->outputs()[1], outputClassList);
	extractTensorAsFloatVector(interpreter.get(), interpreter->outputs()[2], outputScoreList);
	extractTensorAsFloatVector(interpreter.get(), interpreter->outputs()[3], outputNumList);
	int outputNum = (int)outputNumList[0];

	/* Display bbox */
	std::vector<BBox> bboxList;
	getBBox(bboxList, outputBoxList.data(), outputClassList.data(), outputScoreList.data(), outputNum, 0.5, originalImage.cols, originalImage.rows);
	for (int i = 0; i < bboxList.size(); i++) {
		const BBox bbox = bboxList[i];
		cv::rectangle(originalImage, cv::Rect((int)bbox.x, (int)bbox.y, (int)bbox.w, (int)bbox.h), cv::Scalar(255, 255, 0));
		cv::putText(originalImage, labels[bbox.classId], cv::Point((int)bbox.x, (int)bbox.y), cv::FONT_HERSHEY_PLAIN, 1, cv::Scalar(0, 0, 0), 3);
		cv::putText(originalImage, labels[bbox.classId], cv::Point((int)bbox.x, (int)bbox.y), cv::FONT_HERSHEY_PLAIN, 1, cv::Scalar(0, 255, 0), 1);
	}

	glClear(GL_COLOR_BUFFER_BIT);
	glDrawPixels(originalImage.cols, originalImage.rows, GL_RGB, GL_UNSIGNED_BYTE, originalImage.data);
	glFlush();
	const auto& timePost1 = std::chrono::steady_clock::now();

	const auto& timeAll1 = std::chrono::steady_clock::now();
	printf("Total time = %.3lf [msec]\n", (timeAll1 - timeAll0).count() / 1000000.0);
	printf("Capture time = %.3lf [msec]\n", (timeCap1 - timeCap0).count() / 1000000.0);
	printf("Inference time = %.3lf [msec]\n", (timeInference1 - timeInference0).count() / 1000000.0);
	printf("PreProcess time = %.3lf [msec]\n", (timePre1 - timePre0).count() / 1000000.0);
	printf("PostProcess time = %.3lf [msec]\n", (timePost1 - timePost0).count() / 1000000.0);

	const auto& timeFpsNow = std::chrono::steady_clock::now();
	static std::chrono::steady_clock::time_point timeFpsPrevious;
	double callInterval = (timeFpsNow - timeFpsPrevious).count() / 1000000.0;
	printf("FPS = %.1lf [fps], (%.3lf [msec])\n", 1000.0 / callInterval, callInterval);
	printf("========\n");
	timeFpsPrevious = timeFpsNow;
}

static void keyboard(unsigned char key, int x, int y)
{
	switch (key) {
	case 'q':
	case 'Q':
	case '\033':
		glutLeaveMainLoop();
	default:
		break;
	}
}

static void idle(void)
{
	/* call nest display immediately */
	glutPostRedisplay();
}

int main(int argc, char *argv[])
{
	int imageWidth = 720;
	int imageHeight = 480;

	/*** Initialize ***/
	/* read label */
	readLabel(LABEL_NAME, labels);

	/* Create interpreter */
	std::unique_ptr<tflite::FlatBufferModel> model = tflite::FlatBufferModel::BuildFromFile(MODEL_FILENAME);
	TFLITE_MINIMAL_CHECK(model != nullptr);
	tflite::ops::builtin::BuiltinOpResolver resolver;
	tflite::InterpreterBuilder builder(*model, resolver);
	builder(&interpreter);
	TFLITE_MINIMAL_CHECK(interpreter != nullptr);
	interpreter->SetNumThreads(4);
#ifdef USE_EDGETPU_DELEGATE
	size_t num_devices;
	std::unique_ptr<edgetpu_device, decltype(&edgetpu_free_devices)> devices(edgetpu_list_devices(&num_devices), &edgetpu_free_devices);
	TFLITE_MINIMAL_CHECK(num_devices > 0);
	const auto& device = devices.get()[0];
	auto* delegate = edgetpu_create_delegate(device.type, device.path, nullptr, 0);
	interpreter->ModifyGraphWithDelegate({delegate, edgetpu_free_delegate});
#endif
	TFLITE_MINIMAL_CHECK(interpreter->AllocateTensors() == kTfLiteOk);


	/* Get model information */
	displayModelInfo(interpreter.get());
	
	/* initialize camera */
	cap = cv::VideoCapture(0);
	cap.set(cv::CAP_PROP_FRAME_WIDTH, imageWidth);
	cap.set(cv::CAP_PROP_FRAME_HEIGHT, imageHeight);
	// cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('B', 'G', 'R', '3'));
	cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

	/* initialize OpenGL */
	glutInit(&argc, argv);
	glutInitDisplayMode(GLUT_RGB);
	glutInitWindowSize(imageWidth, imageHeight);
	glutCreateWindow("Window");
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluOrtho2D(0.0, imageWidth, 0.0, imageHeight);
	glViewport(0, 0, imageWidth, imageHeight);
	glutDisplayFunc(display);
	glutKeyboardFunc(keyboard);
	glutIdleFunc(idle);

	/*** Start loop ***/
	glutMainLoop();


	model.release();
	interpreter.release();

	return 0;
}
