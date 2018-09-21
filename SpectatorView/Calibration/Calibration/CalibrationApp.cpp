// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.

#include "stdafx.h"
#include "CalibrationApp.h"

#include <iostream>
#include <fstream>
#include <experimental/filesystem>
#include <numeric>

//Get color feed from Hololens using the REST APIs:
//https://developer.microsoft.com/en-us/windows/holographic/device_portal_api_reference
//https://github.com/Microsoft/cpprestsdk/wiki/Getting-Started-Tutorial

CalibrationApp::CalibrationApp() :
    colorTexture(nullptr),
    calibrationPictureElapsedTime(0),
    photoIndex(0),
    availableIndex(0)
{
    deviceResources = std::make_unique<DX::DeviceResources>();
    deviceResources->RegisterDeviceNotify(this);

    InitializeCriticalSection(&photoTextureCriticalSection);
    InitializeCriticalSection(&commandCriticalSection);
    InitializeCriticalSection(&calibrationPictureCriticalSection);
    InitializeCriticalSection(&chessBoardVisualCriticalSection);
    InitializeCriticalSection(&photoVisualCriticalSection);
    InitializeCriticalSection(&imageCopyCriticalSection);

    boardDimensions = cv::Size(GRID_CELLS_X - 1, GRID_CELLS_Y - 1);
    colorBytes = new BYTE[FRAME_BUFSIZE];

    // Force 60fps
    timer.SetFixedTimeStep(true);
    timer.SetTargetElapsedSeconds(1.0 / 60);

    wchar_t myDocumentsPath[1024];
    SHGetFolderPathW(0, CSIDL_MYDOCUMENTS, 0, 0, myDocumentsPath);
    outputPath = std::wstring(myDocumentsPath) + L"\\CalibrationFiles\\";
    cachedOutputPath = std::wstring(myDocumentsPath) + L"\\OldCalibrationFiles\\";

    calibrationFile = outputPath + L"CalibrationData.txt";

    captureText = L"Images captured: %d\nUseable images: %d\nCapture timer: %5.3f\nAvailable Images for Test: %d\n";
    commandText = L"Commands:\nENTER - Perform calibration\nSPACE - Force image capture\nX - Delete captured images\nM - Mirror display\n";
    camPhotoTitleText = L"Camera Image";
    holoPhotoTitleText = L"HoloLens Image";

    DirectoryHelper::CreateOutputDirectory(outputPath);
    DirectoryHelper::CreateOutputDirectory(cachedOutputPath);

    // Move a non-empty output directory to the cached output directory.
    if (!DirectoryHelper::IsDirectoryEmpty(outputPath))
    {
        time_t currentTime = time(0);
        struct tm* t = localtime(&currentTime);

        // Create a archive directory with the current date and time for the existing calibration data.
        std::wstring archiveDirectory = cachedOutputPath + L"\\" +
            std::to_wstring(t->tm_mon + 1) + L"_" +
            std::to_wstring(t->tm_mday) + L"_" +
            std::to_wstring(t->tm_year + 1900) + L"_" +
            std::to_wstring(t->tm_hour) + L"_" +
            std::to_wstring(t->tm_min) + L"_" +
            std::to_wstring(t->tm_sec) + L"\\";

        DirectoryHelper::CreateOutputDirectory(archiveDirectory);
        DirectoryHelper::MoveFiles(outputPath, archiveDirectory);
    }
}

CalibrationApp::~CalibrationApp()
{
    if (frameProvider != nullptr)
    {
        frameProvider->Dispose();
    }
}

// Initialize the Direct3D resources required to run.
void CalibrationApp::Initialize(HWND window, int width, int height)
{
    deviceResources->SetWindow(window, width, height);

    keyboard = std::make_unique<Keyboard>();

    deviceResources->CreateDeviceResources();
    CreateDeviceDependentResources();

    deviceResources->CreateWindowSizeDependentResources();
    CreateWindowSizeDependentResources();

    // Helper Images.
    latestColorMat = cv::Mat(FRAME_HEIGHT, FRAME_WIDTH, CV_8UC4);
    chessBoardVisualMat = cv::Mat(HOLO_HEIGHT, HOLO_WIDTH, CV_8UC4, cv::Scalar(0));
    camPhotoMat = cv::Mat(HOLO_HEIGHT, HOLO_WIDTH, CV_8UC4, cv::Scalar(0));
    holoPhotoMat = cv::Mat(HOLO_HEIGHT, HOLO_WIDTH, CV_8UC4, cv::Scalar(0));
    greenMat = cv::Mat(HOLO_HEIGHT, HOLO_WIDTH, CV_8UC4, cv::Scalar(0, 100, 0, 255));
    redMat = cv::Mat(HOLO_HEIGHT, HOLO_WIDTH, CV_8UC4, cv::Scalar(0, 0, 100, 255));

    // Create an http_client to use REST APIs on the Hololens.
    http_client_config client_config;
    web::credentials cred = web::credentials(HOLOLENS_USER, HOLOLENS_PW);
    client_config.set_credentials(cred);
    client_config.set_validate_certificates(false);
    httpClient = new http_client(HOLOLENS_ADDRESS, client_config);

    // Start the application with no MRC captures on the Hololens.
    DeleteAllMRCFiles();

    // Create textures, RT's and SRV's
    auto device = deviceResources->GetD3DDevice();
    CD3D11_TEXTURE2D_DESC rtDesc(DXGI_FORMAT_R8G8B8A8_UNORM, FRAME_WIDTH, FRAME_HEIGHT,
        1, 1, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    
    device->CreateTexture2D(&rtDesc, nullptr, &colorTexture);
    device->CreateShaderResourceView(colorTexture, nullptr, &srv);

    device->CreateTexture2D(&rtDesc, nullptr, &convertedColorTexture);
    device->CreateRenderTargetView(convertedColorTexture, nullptr, &convertedRT);
    device->CreateShaderResourceView(convertedColorTexture, nullptr, &convertedSrv);

    CD3D11_TEXTURE2D_DESC rtHoloDesc(DXGI_FORMAT_R8G8B8A8_UNORM, HOLO_WIDTH, HOLO_HEIGHT,
        1, 1, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    device->CreateTexture2D(&rtHoloDesc, nullptr, &chessBoardTexture);
    device->CreateShaderResourceView(chessBoardTexture, nullptr, &chessBoardSrv);

    device->CreateTexture2D(&rtHoloDesc, nullptr, &camPhotoTexture);
    device->CreateShaderResourceView(camPhotoTexture, nullptr, &camPhotoSrv);

    device->CreateTexture2D(&rtHoloDesc, nullptr, &holoPhotoTexture);
    device->CreateShaderResourceView(holoPhotoTexture, nullptr, &holoPhotoSrv);

#if USE_ELGATO
    frameProvider = new ElgatoFrameProvider();
#endif
#if USE_DECKLINK || USE_DECKLINK_SHUTTLE
    frameProvider = new DeckLinkManager();
#endif
#if USE_OPENCV
    frameProvider = new OpenCVFrameProvider();
#endif

    // Elgato does not initialize correctly on a background thread.
    if (USE_ELGATO)
    {
        HRESULT hr = E_PENDING;
        while (!SUCCEEDED(hr))
        {
            // Must be done on the main thread.
            hr = frameProvider->Initialize(srv);
            if (FAILED(hr))
            {
                OutputString(L"Failed to initialize frame provider, trying again.\n");
                frameProvider->Dispose();
                Sleep(100);
            }
        }
    }

    yuv2rgbParameters.width = FRAME_WIDTH;
    yuv2rgbParameters.height = FRAME_HEIGHT;

    CD3D11_BUFFER_DESC cbDesc(sizeof(CONVERSION_PARAMETERS), D3D11_BIND_CONSTANT_BUFFER);
    device->CreateBuffer(&cbDesc, nullptr, conversionParamBuffer.ReleaseAndGetAddressOf());

    deviceResources->GetD3DDeviceContext()->UpdateSubresource(conversionParamBuffer.Get(), 0, nullptr,
        &yuv2rgbParameters, sizeof(CONVERSION_PARAMETERS), 0);
}

// Executes the basic game loop.
void CalibrationApp::Tick()
{
    timer.Tick([&]()
    {
        Update(timer);
    });

    Render();
}

// Updates the world.
void CalibrationApp::Update(DX::StepTimer const& timer)
{
    prevKeyState = keyState;
    keyState = keyboard->GetState();

    if (!USE_ELGATO)
    {
        if (frameProvider != nullptr && !frameProvider->IsEnabled())
        {
            if (FAILED(frameProvider->Initialize(srv)))
            {
                return;
            }

            frameProvider->SetOutputTexture(convertedColorTexture);
        }
    }

    // Take calibration pictures at a predetermined interval.
    TakeCalibrationPictureAtInterval(timer);

    // Clear the calibration files that have been generated.
    if (keyState.X && !prevKeyState.X)
    {
        EnterCriticalSection(&commandCriticalSection);
        DeleteOutputFiles();
        LeaveCriticalSection(&commandCriticalSection);
    }

    // Take a calibration picture.
    if (keyState.Space && !prevKeyState.Space)
    {
        pplx::create_task([=]()
        {
            TakeCalibrationPicture();
        });
    }

    // Mirror the image output.
    if (keyState.M && !prevKeyState.M)
    {
        spriteEffect = (spriteEffect == SpriteEffects_None) ?
            SpriteEffects_FlipHorizontally :
            SpriteEffects_None;
    }

    // Use the calibration pictures to stereo calibrate the camera rig.
    if (keyState.Enter && !prevKeyState.Enter)
    {
        EnterCriticalSection(&commandCriticalSection);
        CALIBRATION_RESULTS results;
        PerformCalibration(results, L"");
        LeaveCriticalSection(&commandCriticalSection);
    }

    if (keyState.T && !prevKeyState.T)
    {
        EnterCriticalSection(&commandCriticalSection);
        PerformCalibrationUsingTestData(5, 25, L"c:\\users\\chriba\\documents\\TestCalibrationOutput_5\\");
        PerformCalibrationUsingTestData(10, 25, L"c:\\users\\chriba\\documents\\TestCalibrationOutput_10\\");
        PerformCalibrationUsingTestData(20, 25, L"c:\\users\\chriba\\documents\\TestCalibrationOutput_20\\");
        PerformCalibrationUsingTestData(30, 25, L"c:\\users\\chriba\\documents\\TestCalibrationOutput_30\\");
        PerformCalibrationUsingTestData(50, 25, L"c:\\users\\chriba\\documents\\TestCalibrationOutput_50\\");

        LeaveCriticalSection(&commandCriticalSection);
    }
}

// Take a picture with the camera and hololens.
// There will be approximately 0.2 seconds latency between the two snapshots.
void CalibrationApp::TakeCalibrationPicture()
{
    EnterCriticalSection(&calibrationPictureCriticalSection);
    std::wstring camPath = DirectoryHelper::FindUniqueFileName(outputPath, L"cam", L".png", photoIndex);
    std::wstring holoPath = DirectoryHelper::FindUniqueFileName(outputPath, L"holo", L".jpg", photoIndex);
    int currentIndex = photoIndex;

    // Get latest hologram photo
    std::vector<cv::Point2f> corners;
    cv::Mat grayscaleImage;
    cv::Mat cachedColorMat = cv::Mat(FRAME_HEIGHT, FRAME_WIDTH, CV_8UC4);

    // Cache the latest color frame so we do not stall the UI thread while checking if there is a chess board in frame.
    EnterCriticalSection(&photoTextureCriticalSection);
    memcpy(cachedColorMat.data, latestColorMat.data, FRAME_BUFSIZE);
    LeaveCriticalSection(&photoTextureCriticalSection);

    // Only take the picture if a chessboard is in view.
    if (!HasChessBoard(cachedColorMat, grayscaleImage, corners))
    {
        LeaveCriticalSection(&calibrationPictureCriticalSection);
        return;
    }

    // First take a picture from the Hololens.  This will take about a second.
    TakeMRCPicture();

    // Lock the latest camera image immediately after the hololens picture has been taken.
    EnterCriticalSection(&photoTextureCriticalSection);
    memcpy(cachedColorMat.data, latestColorMat.data, FRAME_BUFSIZE);
    LeaveCriticalSection(&photoTextureCriticalSection);

    // Copy the latest MRC image from the Hololens to the calibration directory.
    GetAllMRCFiles((LPTSTR)holoPath.c_str());
    // Delete all of the MRC files on the device.
    DeleteAllMRCFiles();

    LeaveCriticalSection(&calibrationPictureCriticalSection);

    cv::imwrite(cv::String(StringHelper::ws2s(camPath)), cachedColorMat);

    ProcessChessBoards(currentIndex, cachedColorMat, L"", true);
}

// Take calibration pictures at a predetermined interval.
void CalibrationApp::TakeCalibrationPictureAtInterval(DX::StepTimer const& timer)
{
    // Do not call this function if we do not have a positive non-zero frequency.
    if (CALIBRATION_FREQUENCY_SECONDS <= 0)
    {
        return;
    }

    calibrationPictureElapsedTime += timer.GetElapsedSeconds();
    if (calibrationPictureElapsedTime >= CALIBRATION_FREQUENCY_SECONDS)
    {
        calibrationPictureElapsedTime = 0;

        pplx::create_task([=]()
        {
            TakeCalibrationPicture();
        });
    }
}

// Delete all of the previously taken calibration files and results.
void CalibrationApp::DeleteOutputFiles()
{
    DirectoryHelper::DeleteFiles(outputPath, L".jpg");
    DirectoryHelper::DeleteFiles(outputPath, L".png");
    DirectoryHelper::DeleteFiles(outputPath, L"CalibrationData.txt");

    EnterCriticalSection(&chessBoardVisualCriticalSection);
    chessBoardVisualMat = cv::Mat(HOLO_HEIGHT, HOLO_WIDTH, CV_8UC4, cv::Scalar(0));
    LeaveCriticalSection(&chessBoardVisualCriticalSection);

    EnterCriticalSection(&photoVisualCriticalSection);
    camPhotoMat = cv::Mat(HOLO_HEIGHT, HOLO_WIDTH, CV_8UC4, cv::Scalar(0));
    holoPhotoMat = cv::Mat(HOLO_HEIGHT, HOLO_WIDTH, CV_8UC4, cv::Scalar(0));
    LeaveCriticalSection(&photoVisualCriticalSection);

    stereoObjectPoints.clear();
    stereoColorImagePoints.clear();
    stereoHoloImagePoints.clear();
    colorImagePoints.clear();
    holoImagePoints.clear();
    colorCorners.clear();
    holoCorners.clear();

    photoIndex = 0;
}

// Check if the image has a chessboard in it.
// Output a grayscale converted image and the interior corners of the chessboard.
bool CalibrationApp::HasChessBoard(cv::Mat image, cv::Mat& grayscaleImage, std::vector<cv::Point2f>& corners)
{
    cv::cvtColor(image, grayscaleImage, CV_RGBA2GRAY);

    return cv::findChessboardCorners(grayscaleImage, boardDimensions, corners,
        cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_NORMALIZE_IMAGE + cv::CALIB_CB_FAST_CHECK);
}

// Assesses camera and HoloLens images for chess boards.
void CalibrationApp::ProcessChessBoards(int currentIndex, cv::Mat& colorCameraImage, std::wstring customDirectory, bool copy)
{
    bool validCameraImage = true;
    bool validHoloImage = true;
    std::wstring pathRoot;
    if (customDirectory.empty())
    {
        pathRoot = outputPath + std::to_wstring(currentIndex).c_str() + L"_";
    }
    else
    {
        pathRoot = customDirectory + std::to_wstring(currentIndex).c_str() + L"_";
    }

    std::wstring camPath = pathRoot + L"cam.png";
    std::wstring holPath = pathRoot + L"holo.jpg";

    OutputString((L"Parsing calibration files:\n    " + camPath + L"\n    " + holPath + L"\n").c_str());

    // Get chessboard for DSLR picture
    cv::Mat resizedColorImage_cam;
    cv::resize(colorCameraImage, resizedColorImage_cam, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), 0, 0, cv::INTER_AREA);

    cv::Mat grayscaleImage_cam;;
    if (HasChessBoard(resizedColorImage_cam, grayscaleImage_cam, colorCorners))
    {
        cv::cornerSubPix(grayscaleImage_cam, colorCorners, cv::Size(11, 11), cv::Size(-1, -1),
            cv::TermCriteria(CV_TERMCRIT_EPS + CV_TERMCRIT_ITER, 30, 0.1));
    }
    else
    {
        OutputString((L"ERROR: Chess board not found in " + camPath + L".\n").c_str());
        validCameraImage = false;
    }

    // Load Holo textures
    cv::Mat colorImage_holo = cv::Mat(HOLO_HEIGHT, HOLO_WIDTH, CV_8UC4, cv::Scalar(0));
    if (!DirectoryHelper::FileExists(holPath))
    {
        OutputString((L"ERROR: " + holPath + L" not found.\n").c_str());
        validHoloImage = false;
    }
    else
    {
        colorImage_holo = cv::imread(StringHelper::ws2s(holPath).c_str(), cv::IMREAD_UNCHANGED);
    }

    // Get chess board data from HoloLens
    cv::Mat grayscaleImage_holo;
    if (HasChessBoard(colorImage_holo, grayscaleImage_holo, holoCorners))
    {
        cv::cornerSubPix(grayscaleImage_holo, holoCorners, cv::Size(11, 11), cv::Size(-1, -1),
            cv::TermCriteria(CV_TERMCRIT_EPS + CV_TERMCRIT_ITER, 30, 0.1));
    }
    else
    {
        OutputString((L"ERROR: Chess board not found in " + holPath + L".\n").c_str());
        validHoloImage = false;
    }

    if (validCameraImage && validHoloImage)
    {
        EnterCriticalSection(&commandCriticalSection);
        colorImagePoints.push_back(colorCorners);
        holoImagePoints.push_back(holoCorners);

        // If the entire chess board is found in both images, add this data to the points to calibrate.
        size_t newSize = stereoObjectPoints.size() + 1;
        stereoObjectPoints.resize(newSize);
        stereoColorImagePoints.resize(newSize);
        stereoHoloImagePoints.resize(newSize);

        // Add object-space points for stereo images that both have visible chess boards.
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                stereoObjectPoints[stereoObjectPoints.size() - 1].push_back(
                    cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }

        stereoColorImagePoints[stereoObjectPoints.size() - 1] = colorCorners;
        stereoHoloImagePoints[stereoObjectPoints.size() - 1] = holoCorners;
        LeaveCriticalSection(&commandCriticalSection);

        UpdateChessBoardVisual(colorCorners);
        OutputString((L"Completed parsing calibration files: " + camPath + L", " + holPath + L".\n").c_str());
    }

    EnterCriticalSection(&photoVisualCriticalSection);
    memcpy(camPhotoMat.data, resizedColorImage_cam.data, resizedColorImage_cam.total() * resizedColorImage_cam.elemSize());
    camPhotoMat += validCameraImage ? greenMat : redMat;
    cv::cvtColor(colorImage_holo, holoPhotoMat, CV_BGR2BGRA);
    holoPhotoMat += validHoloImage ? greenMat : redMat;
    LeaveCriticalSection(&photoVisualCriticalSection);

    if (copy && validCameraImage && validHoloImage)
    {
        wchar_t myDocumentsPath[1024];
        SHGetFolderPathW(0, CSIDL_MYDOCUMENTS, 0, 0, myDocumentsPath);
        std::wstring testPath = std::wstring(myDocumentsPath) + L"\\TestCalibrationFiles\\";
        CreateDirectoryW(testPath.c_str(), NULL);

        EnterCriticalSection(&imageCopyCriticalSection);
        std::wstring camTestPath = DirectoryHelper::FindUniqueFileName(testPath, L"cam", L".png", availableIndex);
        std::wstring holoTestPath = DirectoryHelper::FindUniqueFileName(testPath, L"holo", L".jpg", availableIndex);
        bool camSuceeded = CopyFile(camPath.c_str(), camTestPath.c_str(), true);
        bool holoSuceeded = CopyFile(holPath.c_str(), holoTestPath.c_str(), true);
        if (!camSuceeded || !holoSuceeded)
        {
            DeleteFile(camTestPath.c_str());
            DeleteFile(holoTestPath.c_str());
        }
        LeaveCriticalSection(&imageCopyCriticalSection);
    }
}

void CalibrationApp::UpdateChessBoardVisual(std::vector<cv::Point2f>& colorCorners)
{
    // Todo - ask if this seems reasonable
    if (colorCorners.size() != (GRID_CELLS_X - 1) * (GRID_CELLS_Y - 1))
    {
        return;
    }

    // Use first and last points for the top and bottom rows.
    cv::Point tempPoints[1][5];
    tempPoints[0][0] = cv::Point(static_cast<int>(colorCorners[0].x), static_cast<int>(colorCorners[0].y));
    tempPoints[0][1] = cv::Point(static_cast<int>(colorCorners[GRID_CELLS_X - 2].x), static_cast<int>(colorCorners[GRID_CELLS_X - 2].y));
    tempPoints[0][2] = cv::Point(static_cast<int>(colorCorners[(GRID_CELLS_X - 1) * (GRID_CELLS_Y - 1) - 1].x), static_cast<int>(colorCorners[(GRID_CELLS_X - 1) * (GRID_CELLS_Y - 1) - 1].y));
    tempPoints[0][3] = cv::Point(static_cast<int>(colorCorners[(GRID_CELLS_X - 1) * (GRID_CELLS_Y - 2)].x), static_cast<int>(colorCorners[(GRID_CELLS_X - 1) * (GRID_CELLS_Y - 2)].y));
    tempPoints[0][4] = cv::Point(static_cast<int>(colorCorners[0].x), static_cast<int>(colorCorners[0].y));

    const cv::Point* points[1] = { tempPoints[0] };
    const int numPoints[] = { 5 };
    cv::Mat tempMat = cv::Mat(HOLO_HEIGHT, HOLO_WIDTH, CV_8UC4, cv::Scalar(0));
    cv::fillPoly(tempMat, points, numPoints, 1, cv::Scalar(0, 100, 0, 5));

    const cv::Point* linePoints = tempPoints[0];
    cv::polylines(tempMat, &linePoints, numPoints, 1, false, cv::Scalar(0, 0, 100, 255), 2);

    EnterCriticalSection(&chessBoardVisualCriticalSection);
    chessBoardVisualMat += tempMat;
    LeaveCriticalSection(&chessBoardVisualCriticalSection);
}

// Use the calibration pictures to stereo calibrate the camera rig.
void CalibrationApp::PerformCalibration(CALIBRATION_RESULTS& results, std::wstring fileName)
{
    if (colorImagePoints.size() == 0 || holoImagePoints.size() == 0 || stereoColorImagePoints.size() == 0 || stereoHoloImagePoints.size() == 0)
    {
        OutputString(L"ERROR: Please take some valid chess board images before calibration.\n");
    }

    //http://docs.opencv.org/2.4/modules/calib3d/doc/camera_calibration_and_3d_reconstruction.html#Mat initCameraMatrix2D(InputArrayOfArrays objectPoints, InputArrayOfArrays imagePoints, Size imageSize, double aspectRatio)
    // Add object-space points for all camera images.
    std::vector<std::vector<cv::Point3f>> colorObjectPoints;
    colorObjectPoints.resize(colorImagePoints.size());
    for (int colorImagePoint = 0; colorImagePoint< colorImagePoints.size(); colorImagePoint++)
    {
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                colorObjectPoints[colorImagePoint].push_back(
                    cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }
    }

    // Add object-space points for all Hololens images.
    std::vector<std::vector<cv::Point3f>> holoObjectPoints;
    holoObjectPoints.resize(holoImagePoints.size());
    for (int holoImagePoint = 0; holoImagePoint< holoImagePoints.size(); holoImagePoint++)
    {
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                holoObjectPoints[holoImagePoint].push_back(cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }
    }

    double apertureWidth = 0;
    double apertureHeight = 0;
    double holoFovX, holoFovY, colorFovX, colorFovY = 0;
    double focalLength = 0;
    cv::Point2d principalPoint;
    double aspectRatio = 0;

    // Calibrate the individual cameras.
    cv::Mat distCoeffColor, distCoeffHolo;
    cv::Mat colorR, holoR, colorT, holoT;
    
#if DSLR_USE_KNOWN_INTRINSICS
    double colorFocalLength = DSLR_FOCAL_LENGTH * std::min(HOLO_WIDTH / DSLR_MATRIX_WIDTH, HOLO_HEIGHT / DSLR_MATRIX_HEIGHT);
    cv::Mat colorMat = (cv::Mat_<double>(3,3) << colorFocalLength, 0, HOLO_WIDTH / 2., 0, colorFocalLength, HOLO_HEIGHT / 2., 0, 0, 1);
#else
    cv::Mat colorMat = cv::initCameraMatrix2D(colorObjectPoints, colorImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), (double)HOLO_HEIGHT / (double)HOLO_WIDTH);
#endif
    cv::Mat holoMat = cv::initCameraMatrix2D(holoObjectPoints, holoImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), (double)HOLO_HEIGHT / (double)HOLO_WIDTH);

    OutputString(L"Start Calibrating DSLR.\n");
    int colorFlags = CV_CALIB_USE_INTRINSIC_GUESS;
#if DSLR_USE_KNOWN_INTRINSICS
    OutputString(L"Setting user-defined focal length before calibration: ");
    OutputString(std::to_wstring(colorFocalLength).c_str());
    OutputString(L"\n");
#if DSLR_FIX_FOCAL_LENGTH
    colorFlags |= CV_CALIB_FIX_FOCAL_LENGTH;
#endif
#if DSLR_FIX_PRINCIPAL_POINT
    colorFlags |=  CV_CALIB_FIX_PRINCIPAL_POINT;
#endif
#endif
    double colorRMS = cv::calibrateCamera(colorObjectPoints, colorImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), colorMat, distCoeffColor, colorR, colorT, colorFlags);
    OutputString(L"Done Calibrating DSLR.\n");
    OutputString(L"Start Calibrating HoloLens.\n");
    double holoRMS = cv::calibrateCamera(holoObjectPoints, holoImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), holoMat, distCoeffHolo, holoR, holoT, CV_CALIB_USE_INTRINSIC_GUESS);
    OutputString(L"Done Calibrating HoloLens.\n");

    cv::calibrationMatrixValues(holoMat, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), apertureWidth, apertureHeight, holoFovX, holoFovY, focalLength, principalPoint, aspectRatio);
    cv::calibrationMatrixValues(colorMat, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), apertureWidth, apertureHeight, colorFovX, colorFovY, focalLength, principalPoint, aspectRatio);


    // Output rotation, translation, essential matrix, fundamental matrix.
    cv::Mat R, T, E, F;

    //http://docs.opencv.org/2.4/modules/calib3d/doc/camera_calibration_and_3d_reconstruction.html#double stereoCalibrate(InputArrayOfArrays objectPoints, InputArrayOfArrays imagePoints1, InputArrayOfArrays imagePoints2, InputOutputArray cameraMatrix1, InputOutputArray distCoeffs1, InputOutputArray cameraMatrix2, InputOutputArray distCoeffs2, Size imageSize, OutputArray R, OutputArray T, OutputArray E, OutputArray F, TermCriteria criteria, int flags)
    // Stereo calibrate the two cameras.
    OutputString(L"Start stereo calibrating.\n");
    double rms = cv::stereoCalibrate(stereoObjectPoints, stereoHoloImagePoints, stereoColorImagePoints,
        holoMat, distCoeffHolo,
        colorMat, distCoeffColor,
        cv::Size(HOLO_WIDTH, HOLO_HEIGHT),
        R, T, E, F,
        CV_CALIB_FIX_INTRINSIC
    );
    OutputString(L"Done stereo calibrating.\n");

    results.dslrResults.rms = colorRMS;
    results.dslrResults.mat = colorMat.clone();
    results.dslrResults.distortion = distCoeffColor.clone();
    results.dslrResults.fovX = colorFovX;
    results.dslrResults.fovY = colorFovY;

    results.holoResults.rms = holoRMS;
    results.holoResults.mat = holoMat.clone();
    results.holoResults.distortion = distCoeffHolo.clone();
    results.holoResults.fovX = holoFovX;
    results.holoResults.fovY = holoFovY;

    results.stereoRMS = rms;
    results.translation = T.clone();
    results.rotation = R.clone();

    // Write calibration data file:
    // First Delete the old calibration file if one exists.
    if (fileName.empty())
    {
        fileName = calibrationFile;
    }

    DeleteFile(fileName.c_str());

    std::ofstream calibrationfs;
    calibrationfs.open(fileName.c_str());

    calibrationfs << "# Stereo RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "RMS: " << rms << std::endl;

    calibrationfs << "# DSLR RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "DSLR RMS: " << colorRMS << std::endl;

    calibrationfs << "# HoloLens RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "HoloLens RMS: " << holoRMS << std::endl;

    calibrationfs << "# Delta in meters of Hololens from Camera:" << std::endl;
    calibrationfs << "Translation: " << T.at<double>(0, 0) << ", " << T.at<double>(1, 0) << ", " << T.at<double>(2, 0) << std::endl;

    calibrationfs << "# Row Major Matrix3x3 (This should be close to identity)" << std::endl;
    calibrationfs << "Rotation: " << R.at<double>(0, 0) << ", " << R.at<double>(0, 1) << ", " << R.at<double>(0, 2) << ", " <<
        R.at<double>(1, 0) << ", " << R.at<double>(1, 1) << ", " << R.at<double>(1, 2) << ", " << R.at<double>(2, 0) << ", " <<
        R.at<double>(2, 1) << ", " << R.at<double>(2, 2) << std::endl;

    calibrationfs << "# Field of View of DSLR:" << std::endl;
    calibrationfs << "DSLR_fov: " << colorFovX << ", " << colorFovY << std::endl;

    calibrationfs << "# Field of View of HoloLens:" << std::endl;
    calibrationfs << "Holo_fov: " << holoFovX << ", " << holoFovY << std::endl;

    calibrationfs << "# DSLR distortion coefficients:" << std::endl;
    calibrationfs << "DSLR_distortion: " << distCoeffColor.at<double>(0, 0) << ", " << distCoeffColor.at<double>(0, 1) << ", " <<
        distCoeffColor.at<double>(0, 2) << ", " << distCoeffColor.at<double>(0, 3) << ", " << distCoeffColor.at<double>(0, 4) << std::endl;

    calibrationfs << "# DSLR camera Matrix: fx, fy, cx, cy:" << std::endl;
    calibrationfs << "DSLR_camera_Matrix: " << colorMat.at<double>(0, 0) << ", " << colorMat.at<double>(1, 1) << ", " <<
        colorMat.at<double>(0, 2) << ", " << colorMat.at<double>(1, 2) << std::endl;

    calibrationfs << "# HoloLens distortion coefficients:" << std::endl;
    calibrationfs << "Holo_distortion: " << distCoeffHolo.at<double>(0, 0) << ", " << distCoeffHolo.at<double>(0, 1) << ", " <<
        distCoeffHolo.at<double>(0, 2) << ", " << distCoeffHolo.at<double>(0, 3) << ", " << distCoeffHolo.at<double>(0, 4) << std::endl;

    calibrationfs << "# HoloLens camera Matrix: fx, fy, cx, cy:" << std::endl;
    calibrationfs << "Holo_camera_Matrix: " << holoMat.at<double>(0, 0) << ", " << holoMat.at<double>(1, 1) << ", " <<
        holoMat.at<double>(0, 2) << ", " << holoMat.at<double>(1, 2) << std::endl;

    calibrationfs << "# Number of images captured: " << photoIndex << std::endl;
    calibrationfs << "# Number of images used in calibration: " << stereoObjectPoints.size() << std::endl;
    calibrationfs.close();
}

void CalibrationApp::PerformCalibrationHoloMatHoloDistortion(CALIBRATION_RESULTS& results, std::wstring fileName)
{
    if (colorImagePoints.size() == 0 || holoImagePoints.size() == 0 || stereoColorImagePoints.size() == 0 || stereoHoloImagePoints.size() == 0)
    {
        OutputString(L"ERROR: Please take some valid chess board images before calibration.\n");
    }

    //http://docs.opencv.org/2.4/modules/calib3d/doc/camera_calibration_and_3d_reconstruction.html#Mat initCameraMatrix2D(InputArrayOfArrays objectPoints, InputArrayOfArrays imagePoints, Size imageSize, double aspectRatio)
    // Add object-space points for all camera images.
    std::vector<std::vector<cv::Point3f>> colorObjectPoints;
    colorObjectPoints.resize(colorImagePoints.size());
    for (int colorImagePoint = 0; colorImagePoint< colorImagePoints.size(); colorImagePoint++)
    {
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                colorObjectPoints[colorImagePoint].push_back(
                    cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }
    }

    // Add object-space points for all Hololens images.
    std::vector<std::vector<cv::Point3f>> holoObjectPoints;
    holoObjectPoints.resize(holoImagePoints.size());
    for (int holoImagePoint = 0; holoImagePoint< holoImagePoints.size(); holoImagePoint++)
    {
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                holoObjectPoints[holoImagePoint].push_back(cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }
    }

    double apertureWidth = 0;
    double apertureHeight = 0;
    double holoFovX, holoFovY, colorFovX, colorFovY = 0;
    double focalLength = 0;
    cv::Point2d principalPoint;
    double aspectRatio = 0;

    // Calibrate the individual cameras.
    cv::Mat distCoeffColor, distCoeffHolo;
    cv::Mat colorR, holoR, colorT, holoT;

#if DSLR_USE_KNOWN_INTRINSICS
    double colorFocalLength = DSLR_FOCAL_LENGTH * std::min(HOLO_WIDTH / DSLR_MATRIX_WIDTH, HOLO_HEIGHT / DSLR_MATRIX_HEIGHT);
    cv::Mat colorMat = (cv::Mat_<double>(3, 3) << colorFocalLength, 0, HOLO_WIDTH / 2., 0, colorFocalLength, HOLO_HEIGHT / 2., 0, 0, 1);
#else
    cv::Mat colorMat = cv::initCameraMatrix2D(colorObjectPoints, colorImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), (double)HOLO_HEIGHT / (double)HOLO_WIDTH);
#endif

    OutputString(L"Start Calibrating DSLR.\n");
    int colorFlags = CV_CALIB_USE_INTRINSIC_GUESS;
#if DSLR_USE_KNOWN_INTRINSICS
    OutputString(L"Setting user-defined focal length before calibration: ");
    OutputString(std::to_wstring(colorFocalLength).c_str());
    OutputString(L"\n");
#if DSLR_FIX_FOCAL_LENGTH
    colorFlags |= CV_CALIB_FIX_FOCAL_LENGTH;
#endif
#if DSLR_FIX_PRINCIPAL_POINT
    colorFlags |= CV_CALIB_FIX_PRINCIPAL_POINT;
#endif
#endif
    double colorRMS = cv::calibrateCamera(colorObjectPoints, colorImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), colorMat, distCoeffColor, colorR, colorT, colorFlags);
    cv::calibrationMatrixValues(colorMat, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), apertureWidth, apertureHeight, colorFovX, colorFovY, focalLength, principalPoint, aspectRatio);

    OutputString(L"Done Calibrating DSLR.\n");
    OutputString(L"Start Calibrating HoloLens.\n");
    double holoRMS = 0;
    OutputString(L"Done Calibrating HoloLens.\n");

    cv::Mat holoMat = colorMat.clone();
    holoMat.at<double>(0, 0) = 1556.154419f; // fx
    holoMat.at<double>(1, 1) = 1553.992188f; // fy
    holoMat.at<double>(0, 2) = 659.685852; // cx
    holoMat.at<double>(1, 2) = 374.519684; // cy

    distCoeffHolo = distCoeffColor.clone();
    distCoeffHolo.at<double>(0, 0) = 0.192551; // k1
    distCoeffHolo.at<double>(0, 1) = -0.233648; // k2
    distCoeffHolo.at<double>(0, 2) = 0; // r1
    distCoeffHolo.at<double>(0, 3) = 0; // r2
    distCoeffHolo.at<double>(0, 4) = -0.150327; // k3

    cv::calibrationMatrixValues(holoMat, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), apertureWidth, apertureHeight, holoFovX, holoFovY, focalLength, principalPoint, aspectRatio);

    // Output rotation, translation, essential matrix, fundamental matrix.
    cv::Mat R, T, E, F;

    //http://docs.opencv.org/2.4/modules/calib3d/doc/camera_calibration_and_3d_reconstruction.html#double stereoCalibrate(InputArrayOfArrays objectPoints, InputArrayOfArrays imagePoints1, InputArrayOfArrays imagePoints2, InputOutputArray cameraMatrix1, InputOutputArray distCoeffs1, InputOutputArray cameraMatrix2, InputOutputArray distCoeffs2, Size imageSize, OutputArray R, OutputArray T, OutputArray E, OutputArray F, TermCriteria criteria, int flags)
    // Stereo calibrate the two cameras.
    OutputString(L"Start stereo calibrating.\n");
    double rms = cv::stereoCalibrate(stereoObjectPoints, stereoHoloImagePoints, stereoColorImagePoints,
        holoMat, distCoeffHolo,
        colorMat, distCoeffColor,
        cv::Size(HOLO_WIDTH, HOLO_HEIGHT),
        R, T, E, F,
        CV_CALIB_FIX_INTRINSIC
    );
    OutputString(L"Done stereo calibrating.\n");

    results.dslrResults.rms = colorRMS;
    results.dslrResults.mat = colorMat.clone();
    results.dslrResults.distortion = distCoeffColor.clone();
    results.dslrResults.fovX = colorFovX;
    results.dslrResults.fovY = colorFovY;

    results.holoResults.rms = holoRMS;
    results.holoResults.mat = holoMat.clone();
    results.holoResults.distortion = distCoeffHolo.clone();
    results.holoResults.fovX = holoFovX;
    results.holoResults.fovY = holoFovY;

    results.stereoRMS = rms;
    results.translation = T.clone();
    results.rotation = R.clone();

    // Write calibration data file:
    // First Delete the old calibration file if one exists.
    if (fileName.empty())
    {
        fileName = calibrationFile;
    }

    DeleteFile(fileName.c_str());

    std::ofstream calibrationfs;
    calibrationfs.open(fileName.c_str());

    calibrationfs << "# Stereo RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "RMS: " << rms << std::endl;

    calibrationfs << "# DSLR RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "DSLR RMS: " << colorRMS << std::endl;

    calibrationfs << "# HoloLens RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "HoloLens RMS: " << holoRMS << std::endl;

    calibrationfs << "# Delta in meters of Hololens from Camera:" << std::endl;
    calibrationfs << "Translation: " << T.at<double>(0, 0) << ", " << T.at<double>(1, 0) << ", " << T.at<double>(2, 0) << std::endl;

    calibrationfs << "# Row Major Matrix3x3 (This should be close to identity)" << std::endl;
    calibrationfs << "Rotation: " << R.at<double>(0, 0) << ", " << R.at<double>(0, 1) << ", " << R.at<double>(0, 2) << ", " <<
        R.at<double>(1, 0) << ", " << R.at<double>(1, 1) << ", " << R.at<double>(1, 2) << ", " << R.at<double>(2, 0) << ", " <<
        R.at<double>(2, 1) << ", " << R.at<double>(2, 2) << std::endl;

    calibrationfs << "# Field of View of DSLR:" << std::endl;
    calibrationfs << "DSLR_fov: " << colorFovX << ", " << colorFovY << std::endl;

    calibrationfs << "# Field of View of HoloLens:" << std::endl;
    calibrationfs << "Holo_fov: " << holoFovX << ", " << holoFovY << std::endl;

    calibrationfs << "# DSLR distortion coefficients:" << std::endl;
    calibrationfs << "DSLR_distortion: " << distCoeffColor.at<double>(0, 0) << ", " << distCoeffColor.at<double>(0, 1) << ", " <<
        distCoeffColor.at<double>(0, 2) << ", " << distCoeffColor.at<double>(0, 3) << ", " << distCoeffColor.at<double>(0, 4) << std::endl;

    calibrationfs << "# DSLR camera Matrix: fx, fy, cx, cy:" << std::endl;
    calibrationfs << "DSLR_camera_Matrix: " << colorMat.at<double>(0, 0) << ", " << colorMat.at<double>(1, 1) << ", " <<
        colorMat.at<double>(0, 2) << ", " << colorMat.at<double>(1, 2) << std::endl;

    calibrationfs << "# HoloLens distortion coefficients:" << std::endl;
    calibrationfs << "Holo_distortion: " << distCoeffHolo.at<double>(0, 0) << ", " << distCoeffHolo.at<double>(0, 1) << ", " <<
        distCoeffHolo.at<double>(0, 2) << ", " << distCoeffHolo.at<double>(0, 3) << ", " << distCoeffHolo.at<double>(0, 4) << std::endl;

    calibrationfs << "# HoloLens camera Matrix: fx, fy, cx, cy:" << std::endl;
    calibrationfs << "Holo_camera_Matrix: " << holoMat.at<double>(0, 0) << ", " << holoMat.at<double>(1, 1) << ", " <<
        holoMat.at<double>(0, 2) << ", " << holoMat.at<double>(1, 2) << std::endl;

    calibrationfs << "# Number of images captured: " << photoIndex << std::endl;
    calibrationfs << "# Number of images used in calibration: " << stereoObjectPoints.size() << std::endl;
    calibrationfs.close();
}

void CalibrationApp::PerformCalibrationHoloMatNoDistortion(CALIBRATION_RESULTS& results, std::wstring fileName)
{
    if (colorImagePoints.size() == 0 || holoImagePoints.size() == 0 || stereoColorImagePoints.size() == 0 || stereoHoloImagePoints.size() == 0)
    {
        OutputString(L"ERROR: Please take some valid chess board images before calibration.\n");
    }

    //http://docs.opencv.org/2.4/modules/calib3d/doc/camera_calibration_and_3d_reconstruction.html#Mat initCameraMatrix2D(InputArrayOfArrays objectPoints, InputArrayOfArrays imagePoints, Size imageSize, double aspectRatio)
    // Add object-space points for all camera images.
    std::vector<std::vector<cv::Point3f>> colorObjectPoints;
    colorObjectPoints.resize(colorImagePoints.size());
    for (int colorImagePoint = 0; colorImagePoint< colorImagePoints.size(); colorImagePoint++)
    {
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                colorObjectPoints[colorImagePoint].push_back(
                    cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }
    }

    // Add object-space points for all Hololens images.
    std::vector<std::vector<cv::Point3f>> holoObjectPoints;
    holoObjectPoints.resize(holoImagePoints.size());
    for (int holoImagePoint = 0; holoImagePoint< holoImagePoints.size(); holoImagePoint++)
    {
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                holoObjectPoints[holoImagePoint].push_back(cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }
    }

    double apertureWidth = 0;
    double apertureHeight = 0;
    double holoFovX, holoFovY, colorFovX, colorFovY = 0;
    double focalLength = 0;
    cv::Point2d principalPoint;
    double aspectRatio = 0;

    // Calibrate the individual cameras.
    cv::Mat distCoeffColor, distCoeffHolo;
    cv::Mat colorR, holoR, colorT, holoT;

#if DSLR_USE_KNOWN_INTRINSICS
    double colorFocalLength = DSLR_FOCAL_LENGTH * std::min(HOLO_WIDTH / DSLR_MATRIX_WIDTH, HOLO_HEIGHT / DSLR_MATRIX_HEIGHT);
    cv::Mat colorMat = (cv::Mat_<double>(3, 3) << colorFocalLength, 0, HOLO_WIDTH / 2., 0, colorFocalLength, HOLO_HEIGHT / 2., 0, 0, 1);
#else
    cv::Mat colorMat = cv::initCameraMatrix2D(colorObjectPoints, colorImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), (double)HOLO_HEIGHT / (double)HOLO_WIDTH);
#endif

    OutputString(L"Start Calibrating DSLR.\n");
    int colorFlags = CV_CALIB_USE_INTRINSIC_GUESS;
#if DSLR_USE_KNOWN_INTRINSICS
    OutputString(L"Setting user-defined focal length before calibration: ");
    OutputString(std::to_wstring(colorFocalLength).c_str());
    OutputString(L"\n");
#if DSLR_FIX_FOCAL_LENGTH
    colorFlags |= CV_CALIB_FIX_FOCAL_LENGTH;
#endif
#if DSLR_FIX_PRINCIPAL_POINT
    colorFlags |= CV_CALIB_FIX_PRINCIPAL_POINT;
#endif
#endif
    double colorRMS = cv::calibrateCamera(colorObjectPoints, colorImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), colorMat, distCoeffColor, colorR, colorT, colorFlags);
    cv::calibrationMatrixValues(colorMat, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), apertureWidth, apertureHeight, colorFovX, colorFovY, focalLength, principalPoint, aspectRatio);

    OutputString(L"Done Calibrating DSLR.\n");
    OutputString(L"Start Calibrating HoloLens.\n");
    double holoRMS = 0;
    OutputString(L"Done Calibrating HoloLens.\n");

    cv::Mat holoMat = colorMat.clone();
    holoMat.at<double>(0, 0) = 1556.154419f; // fx
    holoMat.at<double>(1, 1) = 1553.992188f; // fy
    holoMat.at<double>(0, 2) = 659.685852; // cx
    holoMat.at<double>(1, 2) = 374.519684; // cy

    distCoeffHolo = distCoeffColor.clone();
    distCoeffHolo.at<double>(0, 0) = 0; // k1
    distCoeffHolo.at<double>(0, 1) = 0; // k2
    distCoeffHolo.at<double>(0, 2) = 0; // r1
    distCoeffHolo.at<double>(0, 3) = 0; // r2
    distCoeffHolo.at<double>(0, 4) = 0; // k3

    cv::calibrationMatrixValues(holoMat, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), apertureWidth, apertureHeight, holoFovX, holoFovY, focalLength, principalPoint, aspectRatio);

    // Output rotation, translation, essential matrix, fundamental matrix.
    cv::Mat R, T, E, F;

    //http://docs.opencv.org/2.4/modules/calib3d/doc/camera_calibration_and_3d_reconstruction.html#double stereoCalibrate(InputArrayOfArrays objectPoints, InputArrayOfArrays imagePoints1, InputArrayOfArrays imagePoints2, InputOutputArray cameraMatrix1, InputOutputArray distCoeffs1, InputOutputArray cameraMatrix2, InputOutputArray distCoeffs2, Size imageSize, OutputArray R, OutputArray T, OutputArray E, OutputArray F, TermCriteria criteria, int flags)
    // Stereo calibrate the two cameras.
    OutputString(L"Start stereo calibrating.\n");
    double rms = cv::stereoCalibrate(stereoObjectPoints, stereoHoloImagePoints, stereoColorImagePoints,
        holoMat, distCoeffHolo,
        colorMat, distCoeffColor,
        cv::Size(HOLO_WIDTH, HOLO_HEIGHT),
        R, T, E, F,
        CV_CALIB_FIX_INTRINSIC
    );
    OutputString(L"Done stereo calibrating.\n");

    results.dslrResults.rms = colorRMS;
    results.dslrResults.mat = colorMat.clone();
    results.dslrResults.distortion = distCoeffColor.clone();
    results.dslrResults.fovX = colorFovX;
    results.dslrResults.fovY = colorFovY;

    results.holoResults.rms = holoRMS;
    results.holoResults.mat = holoMat.clone();
    results.holoResults.distortion = distCoeffHolo.clone();
    results.holoResults.fovX = holoFovX;
    results.holoResults.fovY = holoFovY;

    results.stereoRMS = rms;
    results.translation = T.clone();
    results.rotation = R.clone();

    // Write calibration data file:
    // First Delete the old calibration file if one exists.
    if (fileName.empty())
    {
        fileName = calibrationFile;
    }

    DeleteFile(fileName.c_str());

    std::ofstream calibrationfs;
    calibrationfs.open(fileName.c_str());

    calibrationfs << "# Stereo RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "RMS: " << rms << std::endl;

    calibrationfs << "# DSLR RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "DSLR RMS: " << colorRMS << std::endl;

    calibrationfs << "# HoloLens RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "HoloLens RMS: " << holoRMS << std::endl;

    calibrationfs << "# Delta in meters of Hololens from Camera:" << std::endl;
    calibrationfs << "Translation: " << T.at<double>(0, 0) << ", " << T.at<double>(1, 0) << ", " << T.at<double>(2, 0) << std::endl;

    calibrationfs << "# Row Major Matrix3x3 (This should be close to identity)" << std::endl;
    calibrationfs << "Rotation: " << R.at<double>(0, 0) << ", " << R.at<double>(0, 1) << ", " << R.at<double>(0, 2) << ", " <<
        R.at<double>(1, 0) << ", " << R.at<double>(1, 1) << ", " << R.at<double>(1, 2) << ", " << R.at<double>(2, 0) << ", " <<
        R.at<double>(2, 1) << ", " << R.at<double>(2, 2) << std::endl;

    calibrationfs << "# Field of View of DSLR:" << std::endl;
    calibrationfs << "DSLR_fov: " << colorFovX << ", " << colorFovY << std::endl;

    calibrationfs << "# Field of View of HoloLens:" << std::endl;
    calibrationfs << "Holo_fov: " << holoFovX << ", " << holoFovY << std::endl;

    calibrationfs << "# DSLR distortion coefficients:" << std::endl;
    calibrationfs << "DSLR_distortion: " << distCoeffColor.at<double>(0, 0) << ", " << distCoeffColor.at<double>(0, 1) << ", " <<
        distCoeffColor.at<double>(0, 2) << ", " << distCoeffColor.at<double>(0, 3) << ", " << distCoeffColor.at<double>(0, 4) << std::endl;

    calibrationfs << "# DSLR camera Matrix: fx, fy, cx, cy:" << std::endl;
    calibrationfs << "DSLR_camera_Matrix: " << colorMat.at<double>(0, 0) << ", " << colorMat.at<double>(1, 1) << ", " <<
        colorMat.at<double>(0, 2) << ", " << colorMat.at<double>(1, 2) << std::endl;

    calibrationfs << "# HoloLens distortion coefficients:" << std::endl;
    calibrationfs << "Holo_distortion: " << distCoeffHolo.at<double>(0, 0) << ", " << distCoeffHolo.at<double>(0, 1) << ", " <<
        distCoeffHolo.at<double>(0, 2) << ", " << distCoeffHolo.at<double>(0, 3) << ", " << distCoeffHolo.at<double>(0, 4) << std::endl;

    calibrationfs << "# HoloLens camera Matrix: fx, fy, cx, cy:" << std::endl;
    calibrationfs << "Holo_camera_Matrix: " << holoMat.at<double>(0, 0) << ", " << holoMat.at<double>(1, 1) << ", " <<
        holoMat.at<double>(0, 2) << ", " << holoMat.at<double>(1, 2) << std::endl;

    calibrationfs << "# Number of images captured: " << photoIndex << std::endl;
    calibrationfs << "# Number of images used in calibration: " << stereoObjectPoints.size() << std::endl;
    calibrationfs.close();
}

void CalibrationApp::PerformCalibrationHoloMatOpenCV(CALIBRATION_RESULTS& results, std::wstring fileName)
{
    if (colorImagePoints.size() == 0 || holoImagePoints.size() == 0 || stereoColorImagePoints.size() == 0 || stereoHoloImagePoints.size() == 0)
    {
        OutputString(L"ERROR: Please take some valid chess board images before calibration.\n");
    }

    //http://docs.opencv.org/2.4/modules/calib3d/doc/camera_calibration_and_3d_reconstruction.html#Mat initCameraMatrix2D(InputArrayOfArrays objectPoints, InputArrayOfArrays imagePoints, Size imageSize, double aspectRatio)
    // Add object-space points for all camera images.
    std::vector<std::vector<cv::Point3f>> colorObjectPoints;
    colorObjectPoints.resize(colorImagePoints.size());
    for (int colorImagePoint = 0; colorImagePoint< colorImagePoints.size(); colorImagePoint++)
    {
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                colorObjectPoints[colorImagePoint].push_back(
                    cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }
    }

    // Add object-space points for all Hololens images.
    std::vector<std::vector<cv::Point3f>> holoObjectPoints;
    holoObjectPoints.resize(holoImagePoints.size());
    for (int holoImagePoint = 0; holoImagePoint< holoImagePoints.size(); holoImagePoint++)
    {
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                holoObjectPoints[holoImagePoint].push_back(cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }
    }

    double apertureWidth = 0;
    double apertureHeight = 0;
    double holoFovX, holoFovY, colorFovX, colorFovY = 0;
    double focalLength = 0;
    cv::Point2d principalPoint;
    double aspectRatio = 0;

    // Calibrate the individual cameras.
    cv::Mat distCoeffColor, distCoeffHolo;
    cv::Mat colorR, holoR, colorT, holoT;

#if DSLR_USE_KNOWN_INTRINSICS
    double colorFocalLength = DSLR_FOCAL_LENGTH * std::min(HOLO_WIDTH / DSLR_MATRIX_WIDTH, HOLO_HEIGHT / DSLR_MATRIX_HEIGHT);
    cv::Mat colorMat = (cv::Mat_<double>(3, 3) << colorFocalLength, 0, HOLO_WIDTH / 2., 0, colorFocalLength, HOLO_HEIGHT / 2., 0, 0, 1);
#else
    cv::Mat colorMat = cv::initCameraMatrix2D(colorObjectPoints, colorImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), (double)HOLO_HEIGHT / (double)HOLO_WIDTH);
#endif

    OutputString(L"Start Calibrating DSLR.\n");
    int colorFlags = CV_CALIB_USE_INTRINSIC_GUESS;
#if DSLR_USE_KNOWN_INTRINSICS
    OutputString(L"Setting user-defined focal length before calibration: ");
    OutputString(std::to_wstring(colorFocalLength).c_str());
    OutputString(L"\n");
#if DSLR_FIX_FOCAL_LENGTH
    colorFlags |= CV_CALIB_FIX_FOCAL_LENGTH;
#endif
#if DSLR_FIX_PRINCIPAL_POINT
    colorFlags |= CV_CALIB_FIX_PRINCIPAL_POINT;
#endif
#endif
    double colorRMS = cv::calibrateCamera(colorObjectPoints, colorImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), colorMat, distCoeffColor, colorR, colorT, colorFlags);
    cv::calibrationMatrixValues(colorMat, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), apertureWidth, apertureHeight, colorFovX, colorFovY, focalLength, principalPoint, aspectRatio);

    OutputString(L"Done Calibrating DSLR.\n");
    OutputString(L"Start Calibrating HoloLens.\n");

    cv::Mat holoMat = colorMat.clone();
    holoMat.at<double>(0, 0) = 1556.154419f; // fx
    holoMat.at<double>(1, 1) = 1553.992188f; // fy
    holoMat.at<double>(0, 2) = 659.685852; // cx
    holoMat.at<double>(1, 2) = 374.519684; // cy

    double holoRMS = cv::calibrateCamera(holoObjectPoints, holoImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), holoMat, distCoeffHolo, holoR, holoT, CV_CALIB_USE_INTRINSIC_GUESS);
    OutputString(L"Done Calibrating HoloLens.\n");

    cv::calibrationMatrixValues(holoMat, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), apertureWidth, apertureHeight, holoFovX, holoFovY, focalLength, principalPoint, aspectRatio);

    // Output rotation, translation, essential matrix, fundamental matrix.
    cv::Mat R, T, E, F;

    //http://docs.opencv.org/2.4/modules/calib3d/doc/camera_calibration_and_3d_reconstruction.html#double stereoCalibrate(InputArrayOfArrays objectPoints, InputArrayOfArrays imagePoints1, InputArrayOfArrays imagePoints2, InputOutputArray cameraMatrix1, InputOutputArray distCoeffs1, InputOutputArray cameraMatrix2, InputOutputArray distCoeffs2, Size imageSize, OutputArray R, OutputArray T, OutputArray E, OutputArray F, TermCriteria criteria, int flags)
    // Stereo calibrate the two cameras.
    OutputString(L"Start stereo calibrating.\n");
    double rms = cv::stereoCalibrate(stereoObjectPoints, stereoHoloImagePoints, stereoColorImagePoints,
        holoMat, distCoeffHolo,
        colorMat, distCoeffColor,
        cv::Size(HOLO_WIDTH, HOLO_HEIGHT),
        R, T, E, F,
        CV_CALIB_FIX_INTRINSIC
    );
    OutputString(L"Done stereo calibrating.\n");

    results.dslrResults.rms = colorRMS;
    results.dslrResults.mat = colorMat.clone();
    results.dslrResults.distortion = distCoeffColor.clone();
    results.dslrResults.fovX = colorFovX;
    results.dslrResults.fovY = colorFovY;

    results.holoResults.rms = holoRMS;
    results.holoResults.mat = holoMat.clone();
    results.holoResults.distortion = distCoeffHolo.clone();
    results.holoResults.fovX = holoFovX;
    results.holoResults.fovY = holoFovY;

    results.stereoRMS = rms;
    results.translation = T.clone();
    results.rotation = R.clone();

    // Write calibration data file:
    // First Delete the old calibration file if one exists.
    if (fileName.empty())
    {
        fileName = calibrationFile;
    }

    DeleteFile(fileName.c_str());

    std::ofstream calibrationfs;
    calibrationfs.open(fileName.c_str());

    calibrationfs << "# Stereo RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "RMS: " << rms << std::endl;

    calibrationfs << "# DSLR RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "DSLR RMS: " << colorRMS << std::endl;

    calibrationfs << "# HoloLens RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "HoloLens RMS: " << holoRMS << std::endl;

    calibrationfs << "# Delta in meters of Hololens from Camera:" << std::endl;
    calibrationfs << "Translation: " << T.at<double>(0, 0) << ", " << T.at<double>(1, 0) << ", " << T.at<double>(2, 0) << std::endl;

    calibrationfs << "# Row Major Matrix3x3 (This should be close to identity)" << std::endl;
    calibrationfs << "Rotation: " << R.at<double>(0, 0) << ", " << R.at<double>(0, 1) << ", " << R.at<double>(0, 2) << ", " <<
        R.at<double>(1, 0) << ", " << R.at<double>(1, 1) << ", " << R.at<double>(1, 2) << ", " << R.at<double>(2, 0) << ", " <<
        R.at<double>(2, 1) << ", " << R.at<double>(2, 2) << std::endl;

    calibrationfs << "# Field of View of DSLR:" << std::endl;
    calibrationfs << "DSLR_fov: " << colorFovX << ", " << colorFovY << std::endl;

    calibrationfs << "# Field of View of HoloLens:" << std::endl;
    calibrationfs << "Holo_fov: " << holoFovX << ", " << holoFovY << std::endl;

    calibrationfs << "# DSLR distortion coefficients:" << std::endl;
    calibrationfs << "DSLR_distortion: " << distCoeffColor.at<double>(0, 0) << ", " << distCoeffColor.at<double>(0, 1) << ", " <<
        distCoeffColor.at<double>(0, 2) << ", " << distCoeffColor.at<double>(0, 3) << ", " << distCoeffColor.at<double>(0, 4) << std::endl;

    calibrationfs << "# DSLR camera Matrix: fx, fy, cx, cy:" << std::endl;
    calibrationfs << "DSLR_camera_Matrix: " << colorMat.at<double>(0, 0) << ", " << colorMat.at<double>(1, 1) << ", " <<
        colorMat.at<double>(0, 2) << ", " << colorMat.at<double>(1, 2) << std::endl;

    calibrationfs << "# HoloLens distortion coefficients:" << std::endl;
    calibrationfs << "Holo_distortion: " << distCoeffHolo.at<double>(0, 0) << ", " << distCoeffHolo.at<double>(0, 1) << ", " <<
        distCoeffHolo.at<double>(0, 2) << ", " << distCoeffHolo.at<double>(0, 3) << ", " << distCoeffHolo.at<double>(0, 4) << std::endl;

    calibrationfs << "# HoloLens camera Matrix: fx, fy, cx, cy:" << std::endl;
    calibrationfs << "Holo_camera_Matrix: " << holoMat.at<double>(0, 0) << ", " << holoMat.at<double>(1, 1) << ", " <<
        holoMat.at<double>(0, 2) << ", " << holoMat.at<double>(1, 2) << std::endl;

    calibrationfs << "# Number of images captured: " << photoIndex << std::endl;
    calibrationfs << "# Number of images used in calibration: " << stereoObjectPoints.size() << std::endl;
    calibrationfs.close();
}

void CalibrationApp::PerformCalibrationHoloMatOpenCVFixPrincipal(CALIBRATION_RESULTS& results, std::wstring fileName)
{
    if (colorImagePoints.size() == 0 || holoImagePoints.size() == 0 || stereoColorImagePoints.size() == 0 || stereoHoloImagePoints.size() == 0)
    {
        OutputString(L"ERROR: Please take some valid chess board images before calibration.\n");
    }

    //http://docs.opencv.org/2.4/modules/calib3d/doc/camera_calibration_and_3d_reconstruction.html#Mat initCameraMatrix2D(InputArrayOfArrays objectPoints, InputArrayOfArrays imagePoints, Size imageSize, double aspectRatio)
    // Add object-space points for all camera images.
    std::vector<std::vector<cv::Point3f>> colorObjectPoints;
    colorObjectPoints.resize(colorImagePoints.size());
    for (int colorImagePoint = 0; colorImagePoint< colorImagePoints.size(); colorImagePoint++)
    {
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                colorObjectPoints[colorImagePoint].push_back(
                    cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }
    }

    // Add object-space points for all Hololens images.
    std::vector<std::vector<cv::Point3f>> holoObjectPoints;
    holoObjectPoints.resize(holoImagePoints.size());
    for (int holoImagePoint = 0; holoImagePoint< holoImagePoints.size(); holoImagePoint++)
    {
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                holoObjectPoints[holoImagePoint].push_back(cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }
    }

    double apertureWidth = 0;
    double apertureHeight = 0;
    double holoFovX, holoFovY, colorFovX, colorFovY = 0;
    double focalLength = 0;
    cv::Point2d principalPoint;
    double aspectRatio = 0;

    // Calibrate the individual cameras.
    cv::Mat distCoeffColor, distCoeffHolo;
    cv::Mat colorR, holoR, colorT, holoT;

#if DSLR_USE_KNOWN_INTRINSICS
    double colorFocalLength = DSLR_FOCAL_LENGTH * std::min(HOLO_WIDTH / DSLR_MATRIX_WIDTH, HOLO_HEIGHT / DSLR_MATRIX_HEIGHT);
    cv::Mat colorMat = (cv::Mat_<double>(3, 3) << colorFocalLength, 0, HOLO_WIDTH / 2., 0, colorFocalLength, HOLO_HEIGHT / 2., 0, 0, 1);
#else
    cv::Mat colorMat = cv::initCameraMatrix2D(colorObjectPoints, colorImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), (double)HOLO_HEIGHT / (double)HOLO_WIDTH);
#endif

    OutputString(L"Start Calibrating DSLR.\n");
    int colorFlags = CV_CALIB_USE_INTRINSIC_GUESS;
#if DSLR_USE_KNOWN_INTRINSICS
    OutputString(L"Setting user-defined focal length before calibration: ");
    OutputString(std::to_wstring(colorFocalLength).c_str());
    OutputString(L"\n");
#if DSLR_FIX_FOCAL_LENGTH
    colorFlags |= CV_CALIB_FIX_FOCAL_LENGTH;
#endif
#if DSLR_FIX_PRINCIPAL_POINT
    colorFlags |= CV_CALIB_FIX_PRINCIPAL_POINT;
#endif
#endif
    double colorRMS = cv::calibrateCamera(colorObjectPoints, colorImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), colorMat, distCoeffColor, colorR, colorT, colorFlags);
    cv::calibrationMatrixValues(colorMat, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), apertureWidth, apertureHeight, colorFovX, colorFovY, focalLength, principalPoint, aspectRatio);

    OutputString(L"Done Calibrating DSLR.\n");
    OutputString(L"Start Calibrating HoloLens.\n");

    cv::Mat holoMat = colorMat.clone();
    holoMat.at<double>(0, 0) = 1556.154419f; // fx
    holoMat.at<double>(1, 1) = 1553.992188f; // fy
    holoMat.at<double>(0, 2) = 659.685852; // cx
    holoMat.at<double>(1, 2) = 374.519684; // cy

    double holoRMS = cv::calibrateCamera(holoObjectPoints, holoImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), holoMat, distCoeffHolo, holoR, holoT, CV_CALIB_USE_INTRINSIC_GUESS | CV_CALIB_FIX_PRINCIPAL_POINT);
    OutputString(L"Done Calibrating HoloLens.\n");

    cv::calibrationMatrixValues(holoMat, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), apertureWidth, apertureHeight, holoFovX, holoFovY, focalLength, principalPoint, aspectRatio);

    // Output rotation, translation, essential matrix, fundamental matrix.
    cv::Mat R, T, E, F;

    //http://docs.opencv.org/2.4/modules/calib3d/doc/camera_calibration_and_3d_reconstruction.html#double stereoCalibrate(InputArrayOfArrays objectPoints, InputArrayOfArrays imagePoints1, InputArrayOfArrays imagePoints2, InputOutputArray cameraMatrix1, InputOutputArray distCoeffs1, InputOutputArray cameraMatrix2, InputOutputArray distCoeffs2, Size imageSize, OutputArray R, OutputArray T, OutputArray E, OutputArray F, TermCriteria criteria, int flags)
    // Stereo calibrate the two cameras.
    OutputString(L"Start stereo calibrating.\n");
    double rms = cv::stereoCalibrate(stereoObjectPoints, stereoHoloImagePoints, stereoColorImagePoints,
        holoMat, distCoeffHolo,
        colorMat, distCoeffColor,
        cv::Size(HOLO_WIDTH, HOLO_HEIGHT),
        R, T, E, F,
        CV_CALIB_FIX_INTRINSIC
    );
    OutputString(L"Done stereo calibrating.\n");

    results.dslrResults.rms = colorRMS;
    results.dslrResults.mat = colorMat.clone();
    results.dslrResults.distortion = distCoeffColor.clone();
    results.dslrResults.fovX = colorFovX;
    results.dslrResults.fovY = colorFovY;

    results.holoResults.rms = holoRMS;
    results.holoResults.mat = holoMat.clone();
    results.holoResults.distortion = distCoeffHolo.clone();
    results.holoResults.fovX = holoFovX;
    results.holoResults.fovY = holoFovY;

    results.stereoRMS = rms;
    results.translation = T.clone();
    results.rotation = R.clone();

    // Write calibration data file:
    // First Delete the old calibration file if one exists.
    if (fileName.empty())
    {
        fileName = calibrationFile;
    }

    DeleteFile(fileName.c_str());

    std::ofstream calibrationfs;
    calibrationfs.open(fileName.c_str());

    calibrationfs << "# Stereo RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "RMS: " << rms << std::endl;

    calibrationfs << "# DSLR RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "DSLR RMS: " << colorRMS << std::endl;

    calibrationfs << "# HoloLens RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "HoloLens RMS: " << holoRMS << std::endl;

    calibrationfs << "# Delta in meters of Hololens from Camera:" << std::endl;
    calibrationfs << "Translation: " << T.at<double>(0, 0) << ", " << T.at<double>(1, 0) << ", " << T.at<double>(2, 0) << std::endl;

    calibrationfs << "# Row Major Matrix3x3 (This should be close to identity)" << std::endl;
    calibrationfs << "Rotation: " << R.at<double>(0, 0) << ", " << R.at<double>(0, 1) << ", " << R.at<double>(0, 2) << ", " <<
        R.at<double>(1, 0) << ", " << R.at<double>(1, 1) << ", " << R.at<double>(1, 2) << ", " << R.at<double>(2, 0) << ", " <<
        R.at<double>(2, 1) << ", " << R.at<double>(2, 2) << std::endl;

    calibrationfs << "# Field of View of DSLR:" << std::endl;
    calibrationfs << "DSLR_fov: " << colorFovX << ", " << colorFovY << std::endl;

    calibrationfs << "# Field of View of HoloLens:" << std::endl;
    calibrationfs << "Holo_fov: " << holoFovX << ", " << holoFovY << std::endl;

    calibrationfs << "# DSLR distortion coefficients:" << std::endl;
    calibrationfs << "DSLR_distortion: " << distCoeffColor.at<double>(0, 0) << ", " << distCoeffColor.at<double>(0, 1) << ", " <<
        distCoeffColor.at<double>(0, 2) << ", " << distCoeffColor.at<double>(0, 3) << ", " << distCoeffColor.at<double>(0, 4) << std::endl;

    calibrationfs << "# DSLR camera Matrix: fx, fy, cx, cy:" << std::endl;
    calibrationfs << "DSLR_camera_Matrix: " << colorMat.at<double>(0, 0) << ", " << colorMat.at<double>(1, 1) << ", " <<
        colorMat.at<double>(0, 2) << ", " << colorMat.at<double>(1, 2) << std::endl;

    calibrationfs << "# HoloLens distortion coefficients:" << std::endl;
    calibrationfs << "Holo_distortion: " << distCoeffHolo.at<double>(0, 0) << ", " << distCoeffHolo.at<double>(0, 1) << ", " <<
        distCoeffHolo.at<double>(0, 2) << ", " << distCoeffHolo.at<double>(0, 3) << ", " << distCoeffHolo.at<double>(0, 4) << std::endl;

    calibrationfs << "# HoloLens camera Matrix: fx, fy, cx, cy:" << std::endl;
    calibrationfs << "Holo_camera_Matrix: " << holoMat.at<double>(0, 0) << ", " << holoMat.at<double>(1, 1) << ", " <<
        holoMat.at<double>(0, 2) << ", " << holoMat.at<double>(1, 2) << std::endl;

    calibrationfs << "# Number of images captured: " << photoIndex << std::endl;
    calibrationfs << "# Number of images used in calibration: " << stereoObjectPoints.size() << std::endl;
    calibrationfs.close();
}

void CalibrationApp::PerformCalibrationHoloMatOpenCVZeroTangent(CALIBRATION_RESULTS& results, std::wstring fileName)
{
    if (colorImagePoints.size() == 0 || holoImagePoints.size() == 0 || stereoColorImagePoints.size() == 0 || stereoHoloImagePoints.size() == 0)
    {
        OutputString(L"ERROR: Please take some valid chess board images before calibration.\n");
    }

    //http://docs.opencv.org/2.4/modules/calib3d/doc/camera_calibration_and_3d_reconstruction.html#Mat initCameraMatrix2D(InputArrayOfArrays objectPoints, InputArrayOfArrays imagePoints, Size imageSize, double aspectRatio)
    // Add object-space points for all camera images.
    std::vector<std::vector<cv::Point3f>> colorObjectPoints;
    colorObjectPoints.resize(colorImagePoints.size());
    for (int colorImagePoint = 0; colorImagePoint< colorImagePoints.size(); colorImagePoint++)
    {
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                colorObjectPoints[colorImagePoint].push_back(
                    cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }
    }

    // Add object-space points for all Hololens images.
    std::vector<std::vector<cv::Point3f>> holoObjectPoints;
    holoObjectPoints.resize(holoImagePoints.size());
    for (int holoImagePoint = 0; holoImagePoint< holoImagePoints.size(); holoImagePoint++)
    {
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                holoObjectPoints[holoImagePoint].push_back(cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }
    }

    double apertureWidth = 0;
    double apertureHeight = 0;
    double holoFovX, holoFovY, colorFovX, colorFovY = 0;
    double focalLength = 0;
    cv::Point2d principalPoint;
    double aspectRatio = 0;

    // Calibrate the individual cameras.
    cv::Mat distCoeffColor, distCoeffHolo;
    cv::Mat colorR, holoR, colorT, holoT;

#if DSLR_USE_KNOWN_INTRINSICS
    double colorFocalLength = DSLR_FOCAL_LENGTH * std::min(HOLO_WIDTH / DSLR_MATRIX_WIDTH, HOLO_HEIGHT / DSLR_MATRIX_HEIGHT);
    cv::Mat colorMat = (cv::Mat_<double>(3, 3) << colorFocalLength, 0, HOLO_WIDTH / 2., 0, colorFocalLength, HOLO_HEIGHT / 2., 0, 0, 1);
#else
    cv::Mat colorMat = cv::initCameraMatrix2D(colorObjectPoints, colorImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), (double)HOLO_HEIGHT / (double)HOLO_WIDTH);
#endif

    OutputString(L"Start Calibrating DSLR.\n");
    int colorFlags = CV_CALIB_USE_INTRINSIC_GUESS;
#if DSLR_USE_KNOWN_INTRINSICS
    OutputString(L"Setting user-defined focal length before calibration: ");
    OutputString(std::to_wstring(colorFocalLength).c_str());
    OutputString(L"\n");
#if DSLR_FIX_FOCAL_LENGTH
    colorFlags |= CV_CALIB_FIX_FOCAL_LENGTH;
#endif
#if DSLR_FIX_PRINCIPAL_POINT
    colorFlags |= CV_CALIB_FIX_PRINCIPAL_POINT;
#endif
#endif
    double colorRMS = cv::calibrateCamera(colorObjectPoints, colorImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), colorMat, distCoeffColor, colorR, colorT, colorFlags);
    cv::calibrationMatrixValues(colorMat, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), apertureWidth, apertureHeight, colorFovX, colorFovY, focalLength, principalPoint, aspectRatio);

    OutputString(L"Done Calibrating DSLR.\n");
    OutputString(L"Start Calibrating HoloLens.\n");

    cv::Mat holoMat = colorMat.clone();
    holoMat.at<double>(0, 0) = 1556.154419f; // fx
    holoMat.at<double>(1, 1) = 1553.992188f; // fy
    holoMat.at<double>(0, 2) = 659.685852; // cx
    holoMat.at<double>(1, 2) = 374.519684; // cy

    double holoRMS = cv::calibrateCamera(holoObjectPoints, holoImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), holoMat, distCoeffHolo, holoR, holoT, CV_CALIB_USE_INTRINSIC_GUESS | CV_CALIB_ZERO_TANGENT_DIST);
    OutputString(L"Done Calibrating HoloLens.\n");

    cv::calibrationMatrixValues(holoMat, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), apertureWidth, apertureHeight, holoFovX, holoFovY, focalLength, principalPoint, aspectRatio);

    // Output rotation, translation, essential matrix, fundamental matrix.
    cv::Mat R, T, E, F;

    //http://docs.opencv.org/2.4/modules/calib3d/doc/camera_calibration_and_3d_reconstruction.html#double stereoCalibrate(InputArrayOfArrays objectPoints, InputArrayOfArrays imagePoints1, InputArrayOfArrays imagePoints2, InputOutputArray cameraMatrix1, InputOutputArray distCoeffs1, InputOutputArray cameraMatrix2, InputOutputArray distCoeffs2, Size imageSize, OutputArray R, OutputArray T, OutputArray E, OutputArray F, TermCriteria criteria, int flags)
    // Stereo calibrate the two cameras.
    OutputString(L"Start stereo calibrating.\n");
    double rms = cv::stereoCalibrate(stereoObjectPoints, stereoHoloImagePoints, stereoColorImagePoints,
        holoMat, distCoeffHolo,
        colorMat, distCoeffColor,
        cv::Size(HOLO_WIDTH, HOLO_HEIGHT),
        R, T, E, F,
        CV_CALIB_FIX_INTRINSIC
    );
    OutputString(L"Done stereo calibrating.\n");

    results.dslrResults.rms = colorRMS;
    results.dslrResults.mat = colorMat.clone();
    results.dslrResults.distortion = distCoeffColor.clone();
    results.dslrResults.fovX = colorFovX;
    results.dslrResults.fovY = colorFovY;

    results.holoResults.rms = holoRMS;
    results.holoResults.mat = holoMat.clone();
    results.holoResults.distortion = distCoeffHolo.clone();
    results.holoResults.fovX = holoFovX;
    results.holoResults.fovY = holoFovY;

    results.stereoRMS = rms;
    results.translation = T.clone();
    results.rotation = R.clone();

    // Write calibration data file:
    // First Delete the old calibration file if one exists.
    if (fileName.empty())
    {
        fileName = calibrationFile;
    }

    DeleteFile(fileName.c_str());

    std::ofstream calibrationfs;
    calibrationfs.open(fileName.c_str());

    calibrationfs << "# Stereo RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "RMS: " << rms << std::endl;

    calibrationfs << "# DSLR RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "DSLR RMS: " << colorRMS << std::endl;

    calibrationfs << "# HoloLens RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "HoloLens RMS: " << holoRMS << std::endl;

    calibrationfs << "# Delta in meters of Hololens from Camera:" << std::endl;
    calibrationfs << "Translation: " << T.at<double>(0, 0) << ", " << T.at<double>(1, 0) << ", " << T.at<double>(2, 0) << std::endl;

    calibrationfs << "# Row Major Matrix3x3 (This should be close to identity)" << std::endl;
    calibrationfs << "Rotation: " << R.at<double>(0, 0) << ", " << R.at<double>(0, 1) << ", " << R.at<double>(0, 2) << ", " <<
        R.at<double>(1, 0) << ", " << R.at<double>(1, 1) << ", " << R.at<double>(1, 2) << ", " << R.at<double>(2, 0) << ", " <<
        R.at<double>(2, 1) << ", " << R.at<double>(2, 2) << std::endl;

    calibrationfs << "# Field of View of DSLR:" << std::endl;
    calibrationfs << "DSLR_fov: " << colorFovX << ", " << colorFovY << std::endl;

    calibrationfs << "# Field of View of HoloLens:" << std::endl;
    calibrationfs << "Holo_fov: " << holoFovX << ", " << holoFovY << std::endl;

    calibrationfs << "# DSLR distortion coefficients:" << std::endl;
    calibrationfs << "DSLR_distortion: " << distCoeffColor.at<double>(0, 0) << ", " << distCoeffColor.at<double>(0, 1) << ", " <<
        distCoeffColor.at<double>(0, 2) << ", " << distCoeffColor.at<double>(0, 3) << ", " << distCoeffColor.at<double>(0, 4) << std::endl;

    calibrationfs << "# DSLR camera Matrix: fx, fy, cx, cy:" << std::endl;
    calibrationfs << "DSLR_camera_Matrix: " << colorMat.at<double>(0, 0) << ", " << colorMat.at<double>(1, 1) << ", " <<
        colorMat.at<double>(0, 2) << ", " << colorMat.at<double>(1, 2) << std::endl;

    calibrationfs << "# HoloLens distortion coefficients:" << std::endl;
    calibrationfs << "Holo_distortion: " << distCoeffHolo.at<double>(0, 0) << ", " << distCoeffHolo.at<double>(0, 1) << ", " <<
        distCoeffHolo.at<double>(0, 2) << ", " << distCoeffHolo.at<double>(0, 3) << ", " << distCoeffHolo.at<double>(0, 4) << std::endl;

    calibrationfs << "# HoloLens camera Matrix: fx, fy, cx, cy:" << std::endl;
    calibrationfs << "Holo_camera_Matrix: " << holoMat.at<double>(0, 0) << ", " << holoMat.at<double>(1, 1) << ", " <<
        holoMat.at<double>(0, 2) << ", " << holoMat.at<double>(1, 2) << std::endl;

    calibrationfs << "# Number of images captured: " << photoIndex << std::endl;
    calibrationfs << "# Number of images used in calibration: " << stereoObjectPoints.size() << std::endl;
    calibrationfs.close();
}

void CalibrationApp::PerformCalibrationHoloMatOpenCVFixPrincipalZeroTangent(CALIBRATION_RESULTS& results, std::wstring fileName)
{
    if (colorImagePoints.size() == 0 || holoImagePoints.size() == 0 || stereoColorImagePoints.size() == 0 || stereoHoloImagePoints.size() == 0)
    {
        OutputString(L"ERROR: Please take some valid chess board images before calibration.\n");
    }

    //http://docs.opencv.org/2.4/modules/calib3d/doc/camera_calibration_and_3d_reconstruction.html#Mat initCameraMatrix2D(InputArrayOfArrays objectPoints, InputArrayOfArrays imagePoints, Size imageSize, double aspectRatio)
    // Add object-space points for all camera images.
    std::vector<std::vector<cv::Point3f>> colorObjectPoints;
    colorObjectPoints.resize(colorImagePoints.size());
    for (int colorImagePoint = 0; colorImagePoint< colorImagePoints.size(); colorImagePoint++)
    {
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                colorObjectPoints[colorImagePoint].push_back(
                    cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }
    }

    // Add object-space points for all Hololens images.
    std::vector<std::vector<cv::Point3f>> holoObjectPoints;
    holoObjectPoints.resize(holoImagePoints.size());
    for (int holoImagePoint = 0; holoImagePoint< holoImagePoints.size(); holoImagePoint++)
    {
        for (int i = 0; i < boardDimensions.height; i++)
        {
            for (int j = 0; j < boardDimensions.width; j++)
            {
                holoObjectPoints[holoImagePoint].push_back(cv::Point3f((float)(j * CHESS_SQUARE_SIZE), (float)(i * CHESS_SQUARE_SIZE), 0.0f));
            }
        }
    }

    double apertureWidth = 0;
    double apertureHeight = 0;
    double holoFovX, holoFovY, colorFovX, colorFovY = 0;
    double focalLength = 0;
    cv::Point2d principalPoint;
    double aspectRatio = 0;

    // Calibrate the individual cameras.
    cv::Mat distCoeffColor, distCoeffHolo;
    cv::Mat colorR, holoR, colorT, holoT;

#if DSLR_USE_KNOWN_INTRINSICS
    double colorFocalLength = DSLR_FOCAL_LENGTH * std::min(HOLO_WIDTH / DSLR_MATRIX_WIDTH, HOLO_HEIGHT / DSLR_MATRIX_HEIGHT);
    cv::Mat colorMat = (cv::Mat_<double>(3, 3) << colorFocalLength, 0, HOLO_WIDTH / 2., 0, colorFocalLength, HOLO_HEIGHT / 2., 0, 0, 1);
#else
    cv::Mat colorMat = cv::initCameraMatrix2D(colorObjectPoints, colorImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), (double)HOLO_HEIGHT / (double)HOLO_WIDTH);
#endif

    OutputString(L"Start Calibrating DSLR.\n");
    int colorFlags = CV_CALIB_USE_INTRINSIC_GUESS;
#if DSLR_USE_KNOWN_INTRINSICS
    OutputString(L"Setting user-defined focal length before calibration: ");
    OutputString(std::to_wstring(colorFocalLength).c_str());
    OutputString(L"\n");
#if DSLR_FIX_FOCAL_LENGTH
    colorFlags |= CV_CALIB_FIX_FOCAL_LENGTH;
#endif
#if DSLR_FIX_PRINCIPAL_POINT
    colorFlags |= CV_CALIB_FIX_PRINCIPAL_POINT;
#endif
#endif
    double colorRMS = cv::calibrateCamera(colorObjectPoints, colorImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), colorMat, distCoeffColor, colorR, colorT, colorFlags);
    cv::calibrationMatrixValues(colorMat, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), apertureWidth, apertureHeight, colorFovX, colorFovY, focalLength, principalPoint, aspectRatio);

    OutputString(L"Done Calibrating DSLR.\n");
    OutputString(L"Start Calibrating HoloLens.\n");

    cv::Mat holoMat = colorMat.clone();
    holoMat.at<double>(0, 0) = 1556.154419f; // fx
    holoMat.at<double>(1, 1) = 1553.992188f; // fy
    holoMat.at<double>(0, 2) = 659.685852; // cx
    holoMat.at<double>(1, 2) = 374.519684; // cy

    double holoRMS = cv::calibrateCamera(holoObjectPoints, holoImagePoints, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), holoMat, distCoeffHolo, holoR, holoT, CV_CALIB_USE_INTRINSIC_GUESS | CV_CALIB_FIX_PRINCIPAL_POINT | CV_CALIB_ZERO_TANGENT_DIST);
    OutputString(L"Done Calibrating HoloLens.\n");

    cv::calibrationMatrixValues(holoMat, cv::Size(HOLO_WIDTH, HOLO_HEIGHT), apertureWidth, apertureHeight, holoFovX, holoFovY, focalLength, principalPoint, aspectRatio);

    // Output rotation, translation, essential matrix, fundamental matrix.
    cv::Mat R, T, E, F;

    //http://docs.opencv.org/2.4/modules/calib3d/doc/camera_calibration_and_3d_reconstruction.html#double stereoCalibrate(InputArrayOfArrays objectPoints, InputArrayOfArrays imagePoints1, InputArrayOfArrays imagePoints2, InputOutputArray cameraMatrix1, InputOutputArray distCoeffs1, InputOutputArray cameraMatrix2, InputOutputArray distCoeffs2, Size imageSize, OutputArray R, OutputArray T, OutputArray E, OutputArray F, TermCriteria criteria, int flags)
    // Stereo calibrate the two cameras.
    OutputString(L"Start stereo calibrating.\n");
    double rms = cv::stereoCalibrate(stereoObjectPoints, stereoHoloImagePoints, stereoColorImagePoints,
        holoMat, distCoeffHolo,
        colorMat, distCoeffColor,
        cv::Size(HOLO_WIDTH, HOLO_HEIGHT),
        R, T, E, F,
        CV_CALIB_FIX_INTRINSIC
    );
    OutputString(L"Done stereo calibrating.\n");

    results.dslrResults.rms = colorRMS;
    results.dslrResults.mat = colorMat.clone();
    results.dslrResults.distortion = distCoeffColor.clone();
    results.dslrResults.fovX = colorFovX;
    results.dslrResults.fovY = colorFovY;

    results.holoResults.rms = holoRMS;
    results.holoResults.mat = holoMat.clone();
    results.holoResults.distortion = distCoeffHolo.clone();
    results.holoResults.fovX = holoFovX;
    results.holoResults.fovY = holoFovY;

    results.stereoRMS = rms;
    results.translation = T.clone();
    results.rotation = R.clone();

    // Write calibration data file:
    // First Delete the old calibration file if one exists.
    if (fileName.empty())
    {
        fileName = calibrationFile;
    }

    DeleteFile(fileName.c_str());

    std::ofstream calibrationfs;
    calibrationfs.open(fileName.c_str());

    calibrationfs << "# Stereo RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "RMS: " << rms << std::endl;

    calibrationfs << "# DSLR RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "DSLR RMS: " << colorRMS << std::endl;

    calibrationfs << "# HoloLens RMS calibration error (Lower numbers are better)" << std::endl;
    calibrationfs << "HoloLens RMS: " << holoRMS << std::endl;

    calibrationfs << "# Delta in meters of Hololens from Camera:" << std::endl;
    calibrationfs << "Translation: " << T.at<double>(0, 0) << ", " << T.at<double>(1, 0) << ", " << T.at<double>(2, 0) << std::endl;

    calibrationfs << "# Row Major Matrix3x3 (This should be close to identity)" << std::endl;
    calibrationfs << "Rotation: " << R.at<double>(0, 0) << ", " << R.at<double>(0, 1) << ", " << R.at<double>(0, 2) << ", " <<
        R.at<double>(1, 0) << ", " << R.at<double>(1, 1) << ", " << R.at<double>(1, 2) << ", " << R.at<double>(2, 0) << ", " <<
        R.at<double>(2, 1) << ", " << R.at<double>(2, 2) << std::endl;

    calibrationfs << "# Field of View of DSLR:" << std::endl;
    calibrationfs << "DSLR_fov: " << colorFovX << ", " << colorFovY << std::endl;

    calibrationfs << "# Field of View of HoloLens:" << std::endl;
    calibrationfs << "Holo_fov: " << holoFovX << ", " << holoFovY << std::endl;

    calibrationfs << "# DSLR distortion coefficients:" << std::endl;
    calibrationfs << "DSLR_distortion: " << distCoeffColor.at<double>(0, 0) << ", " << distCoeffColor.at<double>(0, 1) << ", " <<
        distCoeffColor.at<double>(0, 2) << ", " << distCoeffColor.at<double>(0, 3) << ", " << distCoeffColor.at<double>(0, 4) << std::endl;

    calibrationfs << "# DSLR camera Matrix: fx, fy, cx, cy:" << std::endl;
    calibrationfs << "DSLR_camera_Matrix: " << colorMat.at<double>(0, 0) << ", " << colorMat.at<double>(1, 1) << ", " <<
        colorMat.at<double>(0, 2) << ", " << colorMat.at<double>(1, 2) << std::endl;

    calibrationfs << "# HoloLens distortion coefficients:" << std::endl;
    calibrationfs << "Holo_distortion: " << distCoeffHolo.at<double>(0, 0) << ", " << distCoeffHolo.at<double>(0, 1) << ", " <<
        distCoeffHolo.at<double>(0, 2) << ", " << distCoeffHolo.at<double>(0, 3) << ", " << distCoeffHolo.at<double>(0, 4) << std::endl;

    calibrationfs << "# HoloLens camera Matrix: fx, fy, cx, cy:" << std::endl;
    calibrationfs << "Holo_camera_Matrix: " << holoMat.at<double>(0, 0) << ", " << holoMat.at<double>(1, 1) << ", " <<
        holoMat.at<double>(0, 2) << ", " << holoMat.at<double>(1, 2) << std::endl;

    calibrationfs << "# Number of images captured: " << photoIndex << std::endl;
    calibrationfs << "# Number of images used in calibration: " << stereoObjectPoints.size() << std::endl;
    calibrationfs.close();
}

void CalibrationApp::InterpretResults(const std::vector<CALIBRATION_RESULTS>& results, std::wstring& content)
{
    double count = static_cast<double>(results.size());

    content += L"\n";
    double meanRMS = std::accumulate(results.begin(), results.end(), 0.0,
        [count](double value1, CALIBRATION_RESULTS value2)
    {
        return value1 + (value2.stereoRMS / count);
    });
    content += L"STEREO CALIBRATION MEAN RMS: " + std::to_wstring(meanRMS) + L"\n";

    double stdDevRMS = sqrt(std::accumulate(results.begin(), results.end(), 0.0,
        [meanRMS, count](double value1, CALIBRATION_RESULTS value2)
    {
        double diff = meanRMS - value2.stereoRMS;
        double diffSq = diff * diff;
        double val = diffSq / (double)count;
        return value1 + val;
    }));
    stdDevRMS = sqrt(stdDevRMS);
    content += L"STEREO CALIBRATION STDEV RMS: " + std::to_wstring(stdDevRMS) + L"\n";

    cv::Mat meanTranslation = cv::Mat::zeros(results[0].translation.size(), results[0].translation.type());
    for (auto result : results)
    {
        meanTranslation += (result.translation / count);
    }
    content += L"MEAN TRANSLATION: ";
    PrintMat(meanTranslation, content);
    content += L"\n";

    cv::Mat stdevTranslation = cv::Mat::zeros(results[0].translation.size(), results[0].translation.type());
    for (auto result : results)
    {
        auto mat = (meanTranslation - result.translation);
        mat = mat.mul(mat) / count;
        stdevTranslation += mat;
    }
    content += L"STDEV TRANSLATION: ";
    SqrtMat(stdevTranslation);
    PrintMat(stdevTranslation, content);
    content += L"\n";

    cv::Mat meanRotation = cv::Mat::zeros(results[0].rotation.size(), results[0].rotation.type());
    for (auto result : results)
    {
        meanRotation += (result.rotation / count);
    }
    content += L"MEAN ROTATION: ";
    PrintMat(meanRotation, content);
    content += L"\n";

    cv::Mat stdevRotation = cv::Mat::zeros(results[0].rotation.size(), results[0].rotation.type());
    for (auto result : results)
    {
        auto mat = (meanRotation - result.rotation);
        mat = mat.mul(mat) / count;
        stdevRotation += mat;
    }
    content += L"STDEV ROTATION: ";
    SqrtMat(stdevRotation);
    PrintMat(stdevRotation, content);
    content += L"\n";

    content += L"\nDSLR RESULTS:\n";
    double meanDslrRMS = std::accumulate(results.begin(), results.end(), 0.0,
        [count](double value1, CALIBRATION_RESULTS value2)
    {
        return value1 + (value2.dslrResults.rms / count);
    });
    content += L"MEAN DSLR RMS: " + std::to_wstring(meanDslrRMS) + L"\n";

    double stdevDslrRMS = std::accumulate(results.begin(), results.end(), 0.0,
        [meanDslrRMS, count](double value1, CALIBRATION_RESULTS value2)
    {
        double value = meanDslrRMS - value2.dslrResults.rms;
        return value1 + ((value * value) / count);
    });
    stdevDslrRMS = sqrt(stdevDslrRMS);
    content += L"STDEV DSLR RMS: " + std::to_wstring(stdevDslrRMS) + L"\n";

    cv::Mat meanDslrMat = cv::Mat::zeros(results[0].dslrResults.mat.size(), results[0].dslrResults.mat.type());
    for (auto result : results)
    {
        meanDslrMat += (result.dslrResults.mat / count);
    }
    content += L"MEAN DSLR MAT: ";
    PrintMat(meanDslrMat, content);
    content += L"\n";

    cv::Mat stdevDslrMat = cv::Mat::zeros(results[0].dslrResults.mat.size(), results[0].dslrResults.mat.type());
    for (auto result : results)
    {
        auto mat = (meanDslrMat - result.dslrResults.mat);
        mat = mat.mul(mat) / count;
        stdevDslrMat += mat;
    }
    content += L"STDEV DSLR MAT: ";
    SqrtMat(stdevDslrMat);
    PrintMat(stdevDslrMat, content);
    content += L"\n";

    cv::Mat meanDslrDistortion = cv::Mat::zeros(results[0].dslrResults.distortion.size(), results[0].dslrResults.distortion.type());
    for (auto result : results)
    {
        meanDslrDistortion += (result.dslrResults.distortion / count);
    }
    content += L"MEAN DSLR DISTORTION: ";
    PrintMat(meanDslrDistortion, content);
    content += L"\n";

    cv::Mat stdevDslrDistortion = cv::Mat::zeros(results[0].dslrResults.distortion.size(), results[0].dslrResults.distortion.type());
    for (auto result : results)
    {
        auto mat = (meanDslrDistortion - result.dslrResults.distortion);
        mat = mat.mul(mat) / count;
        stdevDslrDistortion += mat;
    }
    content += L"STDEV DSLR DISTORTION: ";
    SqrtMat(stdevDslrDistortion);
    PrintMat(stdevDslrDistortion, content);
    content += L"\n";

    double meanDslrFovX = std::accumulate(results.begin(), results.end(), 0.0,
        [count](double value1, CALIBRATION_RESULTS value2)
    {
        return value1 + (value2.dslrResults.fovX / count);
    });
    content += L"MEAN DSLR FOV X: " + std::to_wstring(meanDslrFovX) + L"\n";

    double stdevDslrFovX = std::accumulate(results.begin(), results.end(), 0.0,
        [meanDslrFovX, count](double value1, CALIBRATION_RESULTS value2)
    {
        double value = meanDslrFovX - value2.dslrResults.fovX;
        return value1 + ((value * value) / count);
    });
    stdevDslrFovX = sqrt(stdevDslrFovX);
    content += L"STDEV DSLR FOV X: " + std::to_wstring(stdevDslrFovX) + L"\n";

    double meanDslrFovY = std::accumulate(results.begin(), results.end(), 0.0,
        [count](double value1, CALIBRATION_RESULTS value2)
    {
        return value1 + (value2.dslrResults.fovY / count);
    });
    content += L"MEAN DSLR FOV Y: " + std::to_wstring(meanDslrFovY) + L"\n";

    double stdevDslrFovY = std::accumulate(results.begin(), results.end(), 0.0,
        [meanDslrFovY, count](double value1, CALIBRATION_RESULTS value2)
    {
        double value = meanDslrFovY - value2.dslrResults.fovY;
        return value1 + ((value * value) / count);
    });
    stdevDslrFovY = sqrt(stdevDslrFovY);
    content += L"STDEV DSLR FOV Y: " + std::to_wstring(stdevDslrFovY) + L"\n";

    content += L"\nHOLOLENS RESULTS:\n";
    double meanHoloRMS = std::accumulate(results.begin(), results.end(), 0.0,
        [count](double value1, CALIBRATION_RESULTS value2)
    {
        return value1 + (value2.holoResults.rms / count);
    });
    content += L"MEAN HOLO RMS: " + std::to_wstring(meanHoloRMS) + L"\n";

    double stdevHoloRMS = std::accumulate(results.begin(), results.end(), 0.0,
        [meanHoloRMS, count](double value1, CALIBRATION_RESULTS value2)
    {
        double value = meanHoloRMS - value2.holoResults.rms;
        return value1 + ((value * value) / count);
    });
    stdevHoloRMS = sqrt(stdevHoloRMS);
    content += L"STDEV HOLO RMS: " + std::to_wstring(stdevHoloRMS) + L"\n";

    cv::Mat meanHoloMat = cv::Mat::zeros(results[0].holoResults.mat.size(), results[0].holoResults.mat.type());
    for (auto result : results)
    {
        meanHoloMat += (result.holoResults.mat / count);
    }
    content += L"MEAN HOLO MAT: ";
    PrintMat(meanHoloMat, content);
    content += L"\n";

    cv::Mat stdevHoloMat = cv::Mat::zeros(results[0].holoResults.mat.size(), results[0].holoResults.mat.type());
    for (auto result : results)
    {
        auto mat = (meanHoloMat - result.holoResults.mat);
        mat = mat.mul(mat) / count;
        stdevHoloMat += mat;
    }
    content += L"STDEV HOLO MAT: ";
    SqrtMat(stdevHoloMat);
    PrintMat(stdevHoloMat, content);
    content += L"\n";

    cv::Mat meanHoloDistortion = cv::Mat::zeros(results[0].holoResults.distortion.size(), results[0].holoResults.distortion.type());
    for (auto result : results)
    {
        meanHoloDistortion += result.holoResults.distortion / count;
    }
    content += L"MEAN HOLO DISTORTION: ";
    PrintMat(meanHoloDistortion, content);
    content += L"\n";

    cv::Mat stdevHoloDistortion = cv::Mat::zeros(results[0].holoResults.distortion.size(), results[0].holoResults.distortion.type());
    for (auto result : results)
    {
        auto mat = (meanHoloDistortion - result.holoResults.distortion);
        mat = mat.mul(mat) / count;
        stdevHoloDistortion += mat;
    }
    content += L"STDEV HOLO DISTORTION: ";
    SqrtMat(stdevHoloDistortion);
    PrintMat(stdevHoloDistortion, content);
    content += L"\n";

    double meanHoloFovX = std::accumulate(results.begin(), results.end(), 0.0,
        [count](double value1, CALIBRATION_RESULTS value2)
    {
        return value1 + (value2.holoResults.fovX / count);
    });
    content += L"MEAN HOLO FOV X: " + std::to_wstring(meanHoloFovX) + L"\n";

    double stdevHoloFovX = std::accumulate(results.begin(), results.end(), 0.0,
        [meanHoloFovX, count](double value1, CALIBRATION_RESULTS value2)
    {
        double value = meanHoloFovX - value2.holoResults.fovX;
        return value1 + ((value * value) / count);
    });
    stdevHoloFovX = sqrt(stdevHoloFovX);
    content += L"STDEV HOLO FOV X: " + std::to_wstring(stdevHoloFovX) + L"\n";

    double meanHoloFovY = std::accumulate(results.begin(), results.end(), 0.0,
        [count](double value1, CALIBRATION_RESULTS value2)
    {
        return value1 + (value2.holoResults.fovY / count);
    });
    content += L"MEAN HOLO FOV Y: " + std::to_wstring(meanHoloFovY) + L"\n";

    double stdevHoloFovY = std::accumulate(results.begin(), results.end(), 0.0,
        [meanHoloFovY, count](double value1, CALIBRATION_RESULTS value2)
    {
        double value = meanHoloFovY - value2.holoResults.fovY;
        return value1 + ((value * value) / count);
    });
    stdevHoloFovY = sqrt(stdevHoloFovY);
    content += L"STDEV HOLO FOV Y: " + std::to_wstring(stdevHoloFovY) + L"\n";
}

void CalibrationApp::PrintMat(const cv::Mat& mat, std::wstring& content)
{
    for (int m = 0; m < mat.rows; m++)
    {
        for (int n = 0; n < mat.cols; n++)
        {
            content += std::to_wstring(mat.at<double>(m, n)) + L" ";
        }
    }
}

void CalibrationApp::SqrtMat(cv::Mat& mat)
{
    for (int m = 0; m < mat.rows; m++)
    {
        for (int n = 0; n < mat.cols; n++)
        {
            mat.at<double>(m, n) = sqrt(mat.at<double>(m, n));
        }
    }
}

void CalibrationApp::PerformCalibrationUsingTestData(int numImages, int numIterations, std::wstring directoryName)
{
    wchar_t myDocumentsPath[1024];
    SHGetFolderPathW(0, CSIDL_MYDOCUMENTS, 0, 0, myDocumentsPath);
    std::wstring testPath = std::wstring(myDocumentsPath) + L"\\TestCalibrationFiles\\";

    int totalImages = 0;
    std::wstring camTestPath = DirectoryHelper::FindUniqueFileName(testPath, L"cam", L".png", totalImages);

    CreateDirectoryW(directoryName.c_str(), NULL);

    std::vector<CALIBRATION_RESULTS> calibrationResults;
    std::vector<CALIBRATION_RESULTS> calibWinrtResults;
    std::vector<CALIBRATION_RESULTS> calibNoDistResults;
    std::vector<CALIBRATION_RESULTS> calibWinrtOpencvResults;
    std::vector<CALIBRATION_RESULTS> calibFixPrincResults;
    std::vector<CALIBRATION_RESULTS> calibZeroTanResults;
    std::vector<CALIBRATION_RESULTS> calibFixPrincZeroTanResults;

    for (int n = 0; n < numIterations; n++)
    {
        std::wstring iterationPrompt = L"Iteration: " + std::to_wstring(n) + L"\n";
        OutputString(iterationPrompt.c_str());

        // Clear any preexisting chess board data
        DeleteOutputFiles();

        std::vector<int> imageIndices;
        for (int i = 0; i < numImages; i++)
        {
            imageIndices.push_back(rand() % totalImages);
        }

        for (auto index : imageIndices)
        {
            std::wstring pathRoot = testPath + std::to_wstring(index).c_str() + L"_";
            std::wstring camPath = pathRoot + L"cam.png";
            auto tempCameraMat = cv::imread(StringHelper::ws2s(camPath).c_str(), cv::IMREAD_UNCHANGED);
            ProcessChessBoards(index, tempCameraMat, testPath, false);
        }

        std::wstring overlayFile = directoryName + std::to_wstring(n) + L"_ChessBoardsUsed.png";
        EnterCriticalSection(&chessBoardVisualCriticalSection);
        cv::imwrite(cv::String(StringHelper::ws2s(overlayFile)), chessBoardVisualMat);
        LeaveCriticalSection(&chessBoardVisualCriticalSection);

        std::wstring calibrationFile = directoryName + std::to_wstring(n) + L"_CalibrationData.txt";
        CALIBRATION_RESULTS results;
        results.imageIndices = imageIndices;
        PerformCalibration(results, calibrationFile);
        calibrationResults.push_back(results);

        calibrationFile = directoryName + std::to_wstring(n) + L"_CalibrationDataWinrt.txt";
        CALIBRATION_RESULTS winrtResults;
        PerformCalibrationHoloMatHoloDistortion(winrtResults, calibrationFile);
        winrtResults.imageIndices = imageIndices;
        calibWinrtResults.push_back(winrtResults);

        calibrationFile = directoryName + std::to_wstring(n) + L"_CalibrationDataNoDist.txt";
        CALIBRATION_RESULTS noDistResults;
        PerformCalibrationHoloMatNoDistortion(noDistResults, calibrationFile);
        noDistResults.imageIndices = imageIndices;
        calibNoDistResults.push_back(noDistResults);

        calibrationFile = directoryName + std::to_wstring(n) + L"_CalibrationDataWinrtOpencv.txt";
        CALIBRATION_RESULTS winrtOpencvResults;
        PerformCalibrationHoloMatOpenCV(winrtOpencvResults, calibrationFile);
        winrtOpencvResults.imageIndices = imageIndices;
        calibWinrtOpencvResults.push_back(winrtOpencvResults);

        calibrationFile = directoryName + std::to_wstring(n) + L"_CalibrationDataFixPrinc.txt";
        CALIBRATION_RESULTS fixPrincResults;
        PerformCalibrationHoloMatOpenCVFixPrincipal(fixPrincResults, calibrationFile);
        fixPrincResults.imageIndices = imageIndices;
        calibFixPrincResults.push_back(fixPrincResults);

        calibrationFile = directoryName + std::to_wstring(n) + L"_CalibrationDataZeroTan.txt";
        CALIBRATION_RESULTS zeroTanResults;
        PerformCalibrationHoloMatOpenCVZeroTangent(zeroTanResults, calibrationFile);
        zeroTanResults.imageIndices = imageIndices;
        calibZeroTanResults.push_back(zeroTanResults);

        calibrationFile = directoryName + std::to_wstring(n) + L"_CalibrationDataFixPrincZeroTan.txt";
        CALIBRATION_RESULTS fixPrincZeroTanResults;
        PerformCalibrationHoloMatOpenCVFixPrincipalZeroTangent(fixPrincZeroTanResults, calibrationFile);
        fixPrincZeroTanResults.imageIndices = imageIndices;
        calibFixPrincZeroTanResults.push_back(fixPrincZeroTanResults);
    }

    std::wstring fileName = directoryName + L"Results.txt";
    DeleteFile(fileName.c_str());
    std::ofstream resultfs;
    resultfs.open(fileName.c_str());

    std::wstring output = L"NUMBER OF IMAGES: " + std::to_wstring(numImages) + L"\n";
    output += L"NUMBER OF ITERATIONS: " + std::to_wstring(numIterations) + L"\n\n";
        
    output += L"OPENCV RESULTS===================================================\n";
    InterpretResults(calibrationResults, output);
    output += L"\n";

    output += L"WINRT RESULTS==================================================\n";
    InterpretResults(calibWinrtResults, output);
    output += L"\n";

    output += L"WINRT NO DISTORTION RESULTS==================================================\n";
    InterpretResults(calibNoDistResults, output);
    output += L"\n";

    output += L"WINRT OPENCV RESULTS==================================================\n";
    InterpretResults(calibWinrtOpencvResults, output);
    output += L"\n";

    output += L"WINRT FIXED PRINCIPLE ==================================================\n";
    InterpretResults(calibFixPrincResults, output);
    output += L"\n";

    output += L"WINRT ZERO TANGENT ==================================================\n";
    InterpretResults(calibZeroTanResults, output);
    output += L"\n";

    output += L"WINRT FIXED PRINCIPLE & ZERO TANGENT ==================================================\n";
    InterpretResults(calibFixPrincZeroTanResults, output);
    output += L"\n";

    resultfs << StringHelper::ws2s(output) << std::endl;
    resultfs.close();
}

void CalibrationApp::Blit(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* dest, ID3D11PixelShader* shader)
{
    auto device = deviceResources->GetD3DDevice();
    auto context = deviceResources->GetD3DDeviceContext();

    ID3D11RenderTargetView* prevRT;
    ID3D11DepthStencilView* prevDepth;
    context->OMGetRenderTargets(1, &prevRT, &prevDepth);

    // Clear out existing render targets
    ID3D11ShaderResourceView *const nullSRV[1] = { nullptr };
    context->PSSetShaderResources(0, 1, nullSRV);

    // Set our new render target.
    context->OMSetRenderTargets(1, &dest, nullptr);
    spriteBatch->Begin(SpriteSortMode_Immediate,
        nullptr, nullptr, nullptr, nullptr,
        [=]() {
        context->PSSetConstantBuffers(0, 1, conversionParamBuffer.GetAddressOf());
        context->PSSetShader(shader, nullptr, 0);
    });

    // Render the source texture to the dest RT with the desired shader.
    spriteBatch->Draw(source, colorSourceRect, &colorSourceRect,
        Colors::White, 0.0f, XMFLOAT2(0, 0), spriteEffect);

    spriteBatch->End();

    // Clear out render target
    context->PSSetShaderResources(0, 1, nullSRV);

    // Reset previous render target.
    context->OMSetRenderTargets(1, &prevRT, prevDepth);
}


// Draws the scene.
void CalibrationApp::Render()
{
    // Don't try to render anything before the first Update.
    if (timer.GetFrameCount() == 0)
    {
        return;
    }

    Clear();

    // Render live camera texture.
    // Get the latest camera frame.
    if (colorTexture != nullptr && frameProvider != nullptr && frameProvider->IsEnabled())
    {
        frameProvider->Update(frameProvider->GetCaptureFrameIndex());

        if (frameProvider->OutputYUV())
        {
            // Convert camera's source yuv image to rgb.
            Blit(srv, convertedRT, yuv2rgbPS.Get());

            // Draw converted image to screen.
            spriteBatch->Begin(SpriteSortMode_Immediate);
            spriteBatch->Draw(convertedSrv, screenRect, &colorSourceRect,
                Colors::White, 0.0f, XMFLOAT2(0, 0), spriteEffect);
            spriteBatch->End();

            // Get bytes from converted image.
            DirectXHelper::GetBytesFromTexture(deviceResources->GetD3DDevice(), convertedColorTexture, FRAME_BPP, colorBytes);
        }
        else
        {
            // Render rgb texture directly.
            spriteBatch->Begin(SpriteSortMode_Immediate,
                nullptr, nullptr, nullptr, nullptr,
                [=]() {
                deviceResources->GetD3DDeviceContext()->PSSetShader(forceOpaquePS.Get(), nullptr, 0);
            });

            spriteBatch->Draw(srv, screenRect, &colorSourceRect,
                Colors::White, 0.0f, XMFLOAT2(0, 0),
                spriteEffect);
            spriteBatch->End();

            // Get bytes from original rgb image.
            DirectXHelper::GetBytesFromTexture(deviceResources->GetD3DDevice(), colorTexture, FRAME_BPP, colorBytes);
        }

        // Cache the latest camera picture in memory to quickly check if there is a chess board in view.
        EnterCriticalSection(&photoTextureCriticalSection);
        latestColorMat.data = colorBytes;
        LeaveCriticalSection(&photoTextureCriticalSection);

        // Draw observed chess boards visual.
        if (TryEnterCriticalSection(&chessBoardVisualCriticalSection))
        {
            DirectXHelper::UpdateSRV(deviceResources->GetD3DDevice(), chessBoardSrv, chessBoardVisualMat.data, HOLO_WIDTH * 4);
            LeaveCriticalSection(&chessBoardVisualCriticalSection);
        }

        overlaySpriteBatch->Begin(SpriteSortMode_Immediate);
        overlaySpriteBatch->Draw(chessBoardSrv, screenRect, &holoDimRect,
            Colors::White, 0.0f, XMFLOAT2(0, 0),
            spriteEffect);
        overlaySpriteBatch->End();

        // Draw camera and holo images.
        if (TryEnterCriticalSection(&photoVisualCriticalSection))
        {
            DirectXHelper::UpdateSRV(deviceResources->GetD3DDevice(), camPhotoSrv, camPhotoMat.data, HOLO_WIDTH * 4);
            DirectXHelper::UpdateSRV(deviceResources->GetD3DDevice(), holoPhotoSrv, holoPhotoMat.data, HOLO_WIDTH * 4);
            LeaveCriticalSection(&photoVisualCriticalSection);
        }

        spriteBatch->Begin(SpriteSortMode_Immediate,
            nullptr, nullptr, nullptr, nullptr,
            [=]() {
            deviceResources->GetD3DDeviceContext()->PSSetShader(forceOpaquePS.Get(), nullptr, 0);
        });
        spriteBatch->Draw(camPhotoSrv, camPhotoRect, &holoDimRect,
            Colors::White, 0.0f, XMFLOAT2(0, 0),
            spriteEffect);
        spriteBatch->Draw(holoPhotoSrv, holoPhotoRect, &holoDimRect,
            Colors::White, 0.0f, XMFLOAT2(0, 0),
            spriteEffect);
        spriteBatch->End();

        // Draw capture text.
        textSpriteBatch->Begin();
        wchar_t tempBuffer[256];
        swprintf(tempBuffer, 256, captureText.c_str(),
            photoIndex,
            stereoObjectPoints.size(),
            (CALIBRATION_FREQUENCY_SECONDS - calibrationPictureElapsedTime),
            availableIndex);
        spriteFont->DrawString(textSpriteBatch.get(), tempBuffer, XMFLOAT2(1.f, 1.f), Colors::Black);
        spriteFont->DrawString(textSpriteBatch.get(), tempBuffer, XMFLOAT2(0, 0), Colors::White);

        // Draw command text.
        auto textRect = spriteFont->MeasureDrawBounds(commandText.c_str(), XMFLOAT2(0, 0));
        auto yOffset = static_cast<float>(screenRect.bottom - (textRect.bottom - textRect.top + 30));
        spriteFont->DrawString(textSpriteBatch.get(), commandText.c_str(), XMFLOAT2(1.f, yOffset + 1.f), Colors::Black);
        spriteFont->DrawString(textSpriteBatch.get(), commandText.c_str(), XMFLOAT2(0, yOffset), Colors::White);

        // Draw camera photo title text.
        auto cameraPhotoXOffset = static_cast<float>(camPhotoRect.left);
        auto cameraPhotoYOffset = static_cast<float>(screenRect.bottom);
        spriteFont->DrawString(textSpriteBatch.get(), camPhotoTitleText.c_str(), XMFLOAT2(cameraPhotoXOffset + 1.f, cameraPhotoYOffset + 1.f), Colors::Black);
        spriteFont->DrawString(textSpriteBatch.get(), camPhotoTitleText.c_str(), XMFLOAT2(cameraPhotoXOffset, cameraPhotoYOffset), Colors::White);

        // Draw HoloLens photo title text.
        auto holoPhotoXOffset = static_cast<float>(holoPhotoRect.left);
        auto holoPhotoYOffset = static_cast<float>(screenRect.bottom);
        spriteFont->DrawString(textSpriteBatch.get(), holoPhotoTitleText.c_str(), XMFLOAT2(holoPhotoXOffset + 1.f, holoPhotoYOffset + 1.f), Colors::Black);
        spriteFont->DrawString(textSpriteBatch.get(), holoPhotoTitleText.c_str(), XMFLOAT2(holoPhotoXOffset, holoPhotoYOffset), Colors::White);
        textSpriteBatch->End();

        deviceResources->Present();
    }
}

// Helper method to clear the back buffers.
void CalibrationApp::Clear()
{
    // Clear the views
    auto context = deviceResources->GetD3DDeviceContext();
    auto renderTarget = deviceResources->GetBackBufferRenderTargetView();
    auto depthStencil = deviceResources->GetDepthStencilView();

    context->ClearRenderTargetView(renderTarget, Colors::CornflowerBlue);
    context->ClearDepthStencilView(depthStencil, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    context->OMSetRenderTargets(1, &renderTarget, depthStencil);

    // Set the viewport.
    auto viewport = deviceResources->GetScreenViewport();
    context->RSSetViewports(1, &viewport);
}

// Message handlers
void CalibrationApp::OnActivated()
{
    // TODO: Game is becoming active window.
}

void CalibrationApp::OnDeactivated()
{
    // TODO: Game is becoming background window.
}

void CalibrationApp::OnSuspending()
{
    // TODO: Game is being power-suspended (or minimized).
}

void CalibrationApp::OnResuming()
{
    timer.ResetElapsedTime();

    // TODO: Game is being power-resumed (or returning from minimize).
}

void CalibrationApp::OnWindowSizeChanged(int width, int height)
{
    // Do not resize anything here, the presentation is just to visualize that the calibration card is in frame.
    // All calibration calculations will be done on original texture dimensions or aspect ratios.
    return;
}

// Properties
void CalibrationApp::GetDefaultSize(int& width, int& height) const
{
    width = FRAME_WIDTH;
    height = (int) (1.5 * FRAME_HEIGHT);
}

// These are the resources that depend on the device.
void CalibrationApp::CreateDeviceDependentResources()
{
    auto device = deviceResources->GetD3DDevice();
    auto context = deviceResources->GetD3DDeviceContext();

    spriteBatch = std::make_unique<SpriteBatch>(context);
    overlaySpriteBatch = std::make_unique<SpriteBatch>(context);
    textSpriteBatch = std::make_unique<SpriteBatch>(context);
    spriteFont = std::make_unique<SpriteFont>(device, L"segoeUI32.spritefont");

    // Camera
    colorSourceRect.left = 0;
    colorSourceRect.top = 0;
    colorSourceRect.right = FRAME_WIDTH;
    colorSourceRect.bottom = FRAME_HEIGHT;

    // Chess Board Visual, Captured Photos
    holoDimRect.left = 0;
    holoDimRect.top = 0;
    holoDimRect.right = HOLO_WIDTH;
    holoDimRect.bottom = HOLO_HEIGHT;

    auto blob = DX::ReadData(L"YUV2RGB.cso");
    HRESULT hr = device->CreatePixelShader(blob.data(), blob.size(),
        nullptr, yuv2rgbPS.ReleaseAndGetAddressOf());
    if FAILED(hr)
    {
        OutputString(L"Error compiling yuv2rgb shader.");
    }

    blob = DX::ReadData(L"ForceOpaque.cso");
    hr = device->CreatePixelShader(blob.data(), blob.size(),
        nullptr, forceOpaquePS.ReleaseAndGetAddressOf());
    if FAILED(hr)
    {
        OutputString(L"Error compiling ForceOpaque shader.");
    }
}

// Allocate all memory resources that change on a window SizeChanged event.
void CalibrationApp::CreateWindowSizeDependentResources()
{
    screenRect.left = 0;
    screenRect.top = 0;
    screenRect.right = static_cast<LONG>(deviceResources->GetScreenViewport().Width);
    screenRect.bottom = static_cast<LONG>(2.0f * deviceResources->GetScreenViewport().Height / 3.0f);

    camPhotoRect.left = 0;
    camPhotoRect.top = screenRect.bottom;
    camPhotoRect.right = static_cast<LONG>(deviceResources->GetScreenViewport().Width / 2.0f);
    camPhotoRect.bottom = static_cast<LONG>(deviceResources->GetScreenViewport().Height);

    holoPhotoRect.left = camPhotoRect.right;
    holoPhotoRect.top = screenRect.bottom;
    holoPhotoRect.right = static_cast<LONG>(deviceResources->GetScreenViewport().Width);
    holoPhotoRect.bottom = static_cast<LONG>(deviceResources->GetScreenViewport().Height);
}

void CalibrationApp::OnDeviceLost()
{
    // TODO: Add Direct3D resource cleanup here.
    spriteBatch.reset();
    overlaySpriteBatch.reset();
    textSpriteBatch.reset();
    spriteFont.reset();

    frameProvider->Dispose();
    SafeRelease(srv);

    yuv2rgbPS.Reset();
    forceOpaquePS.Reset();
}

void CalibrationApp::OnDeviceRestored()
{
    CreateDeviceDependentResources();

    CreateWindowSizeDependentResources();
}

// Take a picture from the Hololens.
void CalibrationApp::TakeMRCPicture()
{
    uri_builder builder(U("/api/holographic/mrc/photo"));
    builder.append_query(U("pv"), U("true"));
    builder.append_query(U("holo"), U("false"));

    try
    {
        httpClient->request(methods::POST, builder.to_string()).wait();
    }
    catch (const std::exception &e)
    {
        OutputString(L"Error taking MRC Photo: ");
        OutputString(StringHelper::s2ws(e.what()).c_str());
        OutputString(L"\n");
    }
}

// Delete all of the MRC files on the Hololens.
void CalibrationApp::DeleteAllMRCFiles()
{
    uri_builder builder(U("/api/holographic/mrc/files"));

    try
    {
        auto task = httpClient->request(methods::GET, builder.to_string()).then([](http_response response) -> pplx::task<json::value>
        {
            // If we get a valid response, return the extracted json. 
            if (response.status_code() == status_codes::OK)
            {
                return response.extract_json();
            }

            // Otherwise return empty json. 
            return pplx::task_from_result(json::value());
        })
            .then([=](pplx::task<json::value> previousTask)
        {
            const json::value& jv = previousTask.get();

            // Iterate over the json to get the MRC file names.
            for (auto iter = jv.as_object().cbegin(); iter != jv.as_object().cend(); iter++)
            {
                // Iterate over values in children to find the filename key.
                json::array children = iter->second.as_array();

                for (int i = 0; i < children.size(); i++)
                {
                    for (auto iter2 = children[i].as_object().cbegin(); iter2 != children[i].as_object().cend(); iter2++)
                    {
                        if (iter2->first == L"FileName")
                        {
                            // Convert this filename to base64.
                            LPTSTR encodedString;
                            StringHelper::base64_encode(StringHelper::ws2s(iter2->second.as_string()), encodedString);
                            DeleteMRCFile(encodedString);
                            HeapFree(GetProcessHeap(), HEAP_NO_SERIALIZE, encodedString);
                        }
                    }
                }
            }
        });

        // Wait for deletion before returning.
        task.wait();
    }
    catch (const std::exception &e)
    {
        OutputString(L"Error deleting MRC files: ");
        OutputString(StringHelper::s2ws(e.what()).c_str());
        OutputString(L"\n");
    }
}

// Delete a particular MRC file from the Hololens.  FileName must be base64 encoded.
void CalibrationApp::DeleteMRCFile(LPTSTR fileName)
{
    uri_builder builder(U("/api/holographic/mrc/file"));
    builder.append_query(U("filename"), fileName);

    try
    {
        httpClient->request(methods::DEL, builder.to_string()).get();
    }
    catch (const std::exception &e)
    {
        OutputString(L"Error Deleting MRC file: ");
        OutputString(StringHelper::s2ws(e.what()).c_str());
        OutputString(L"\n");
    }
}

// Get all of the MRC files on the Hololens and copy to the input path.
void CalibrationApp::GetAllMRCFiles(LPTSTR holoPath)
{
    uri_builder builder(U("/api/holographic/mrc/files"));

    try
    {
        auto task = httpClient->request(methods::GET, builder.to_string()).then([](http_response response) -> pplx::task<json::value>
        {
            // If we get a valid response, return the extracted json. 
            if (response.status_code() == status_codes::OK)
            {
                return response.extract_json();
            }

            // Otherwise return empty json. 
            return pplx::task_from_result(json::value());
        })
            .then([=](pplx::task<json::value> previousTask)
        {
            const json::value& jv = previousTask.get();

            // Iterate over the json to get the MRC file names.
            for (auto iter = jv.as_object().cbegin(); iter != jv.as_object().cend(); iter++)
            {
                // Iterate over values in children to find the filename key.
                json::array children = iter->second.as_array();

                for (int i = 0; i < children.size(); i++)
                {
                    for (auto iter2 = children[i].as_object().cbegin(); iter2 != children[i].as_object().cend(); iter2++)
                    {
                        if (iter2->first == L"FileName")
                        {
                            // Convert this filename to base64.
                            LPTSTR encodedString;
                            StringHelper::base64_encode(StringHelper::ws2s(iter2->second.as_string()), encodedString);
                            GetMRCFile(encodedString, holoPath);
                            HeapFree(GetProcessHeap(), HEAP_NO_SERIALIZE, encodedString);
                        }
                    }
                }
            }
        });

        // Wait for files to copy before returning.
        task.wait();
    }
    catch (const std::exception &e)
    {
        OutputString(L"Error Getting MRC files: ");
        OutputString(StringHelper::s2ws(e.what()).c_str());
        OutputString(L"\n");
    }
}

// Copy a particular MRC file from the Hololens to the local PC.  FileName must be base64 encoded.
void CalibrationApp::GetMRCFile(LPTSTR fileName, LPTSTR holoPath)
{
    try
    {
        auto fileStream = std::make_shared<concurrency::streams::ostream>();

        // Open stream to output file.
        pplx::task<void> requestTask = concurrency::streams::fstream::open_ostream(holoPath).then([=](concurrency::streams::ostream outFile)
        {
            *fileStream = outFile;

            uri_builder builder(U("/api/holographic/mrc/file"));
            builder.append_query(U("filename"), fileName);
            builder.append_query(U("op"), U("stream"));

            return httpClient->request(methods::GET, builder.to_string()).get();
        })

            // Handle response headers arriving.
            .then([=](http_response response)
        {
            // Write response body into the file.
            return response.body().read_to_end(fileStream->streambuf());
        })

            // Close the file stream.
            .then([=](size_t)
        {
            return fileStream->close().get();
        });

        // Wait for file copy to complete before returning.
        requestTask.wait();
    }
    catch (const std::exception &e)
    {
        OutputString(L"Error Getting MRC file: ");
        OutputString(StringHelper::s2ws(e.what()).c_str());
        OutputString(L"\n");
    }
}