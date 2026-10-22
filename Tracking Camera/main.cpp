#include <SFML/Graphics.hpp>
#include <iostream>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <math.h>

#include <string>
#include <source_location> // https://en.cppreference.com/w/cpp/utility/source_location
#include <filesystem>
#include <thread>

//Plans
/*
Make a list of keyed colors to track multiple trackers at the same time. Make a setup phase.

*/

//CONTROLS  
/*
L = toggle cursor lock/keying vs tracking mode. Keying mode will check every pixel, tracking only the cross sections
J+K = decrease/increase hue threshold
N+M = decrease/increase luminance threshold
I+O = decrease/increase keyed color kernel size
U = toggle fullscreen
Left Mouse = pick color
Escape (while in keying mode) = exit
*/
//For slightly enhanced debugging
std::ostream& dump_location(std::source_location loc = std::source_location::current())
{
    return std::cout << std::filesystem::path( loc.file_name() ).filename() << ", function: "
                     << loc.function_name() << ", line: " << loc.line() << '\n' ;
}


sf::Vector2u getCoord(size_t n_, const unsigned int width_, const unsigned int height_){
        unsigned int y = n_ / width_;
        unsigned int x = n_ - y*width_;
    
        return sf::Vector2u{x,y};
}

float clampFloat(const float& input, const float& min, const float& max){
    return min*(input<min) + max*(input>max) + input*((input>=min)&&(input<=max));
}


float absFloat(float input){
if (input<0.f) return -input; else return input;
}


sf::Vector3f getColorRatioMulti(const sf::Color&);

float getLuminanceRatio(const sf::Color& color){
    return (color.r + color.g + color.b)/765.f;//255*3
}

bool getColorRatiosMatch(const sf::Color& color1, const sf::Color& color2, const float& colorDifferenceThreshold, const float& luminanceDifferenceThreshold = 1.f){

    sf::Vector3f ratio1 = getColorRatioMulti(color1);
    sf::Vector3f ratio2 = getColorRatioMulti(color2);
            
    const float luminanceRatio1 = getLuminanceRatio(color1);
    const float luminanceRatio2 = getLuminanceRatio(color2);

    const float luminanceDifference = abs(luminanceRatio1-luminanceRatio2);

    float colorRatioDifference = (absFloat(ratio1.x-ratio2.x) + absFloat(ratio1.y-ratio2.y) + absFloat(ratio1.z-ratio2.z))/3.f;
    if ((colorRatioDifference <= colorDifferenceThreshold) && (luminanceDifference <= luminanceDifferenceThreshold))
    return 1;

    return 0;

}

sf::Vector3f getColorRatioMulti(const sf::Color& color){
        std::uint16_t total = color.r + color.g + color.b;
        if (total==0)//to provide value output for 0,0,0
        return sf::Vector3f{0,0,0};
            else
        return sf::Vector3f{(float)color.r/total,(float)color.g/total,(float)color.b/total};
}


constexpr float quarterPi = 0.785398163f;

int main() {

auto trackingLoop = []()
{
    gst_init(nullptr, nullptr);

    unsigned targetFramerate = 70;//130;

    bool YUYVmode = 1;
    GstElement *pipeline = nullptr;
    if (YUYVmode)
    // Build pipeline: camera → convert → appsink
    //pipeline = gst_parse_launch("v4l2src device=/dev/video0 ! videoconvert ! video/x-raw,format=RGBA ! appsink name=sink", nullptr);
 pipeline = gst_parse_launch(
    "v4l2src device=/dev/video0 !"
    "image/jpeg,format=YUYV,width=1920,height=1080,framerate=60/1 ! "//120
    "jpegdec ! videoconvert ! video/x-raw,format=RGBA ! "
    "appsink name=sink"

, nullptr);

    else    
    pipeline = gst_parse_launch(
    "v4l2src device=/dev/video0 ! "
    "image/jpeg,format=MJPG,width=1920,height=1080,framerate=60/1 ! "
    "jpegdec ! videoconvert ! video/x-raw,format=RGBA ! "
    "appsink name=sink",
    nullptr);



    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    gst_app_sink_set_max_buffers (GST_APP_SINK(sink), 1);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    std::string windowName = "App";
    bool fullscreenEnabled = 1;
    sf::Vector2u windowResolution{1920,1080};
    sf::RenderWindow window(sf::VideoMode({100,100}), windowName,sf::State::Windowed);
    

    if (fullscreenEnabled)
    window.create(sf::VideoMode(windowResolution), windowName,sf::State::Fullscreen);
        else
    window.create(sf::VideoMode(windowResolution), windowName,sf::State::Windowed);


    window.setFramerateLimit(0);
    window.setVerticalSyncEnabled(0);
 
    std::uint8_t noiseThreshold = 25;//25;//20
    
    //Use N and M
    float luminanceThreshold = 0.1f;//10%

    //Use J and K
    float colorThreshold = 0.05f;//now 10%

    float minDiameterPercentThreshold = 0.05f*0+1.f/640.f*10;//5% of the width of the video feed is the minimum size for the tracker
    float referenceDistanceInches = 6.f;
    unsigned int nCalibrationPixelsArea = 175;

    bool pickLock = 1;
    bool pickLockKeyPrev = 0;

    float cursorSensitivity = 100.f;


    //to eliminate jittering with low movement (detection)
    //the tradeoff is, higher sensitivty, less stability, and visa versa
    //500 is recommended for a more sensitive setup, better for further away targets (because they move less relative to the camera)
    //1000 is recommended for closer up targets and overall better stability
    unsigned int minimumDetectedPixels = 50;//50//500//1000//2000 for extreme stability
    
    unsigned int localDetectOverrideLimit = 5;//5//100 pixel radius from last position

    bool jitterProtection = 1;//recommended on
    bool rangeProtection = 0;//deprecated//recommended (or necessary to be) off! It was used to kinda fix the same jittery problem with low movement
    //*********************************************************************************************
    
    //force window focus for the app to run
    bool forceWindowFocus = 0;
    //flip the point position for use as a mouse cursor
    bool screenCorrectedCursor = 1;

    //makes cursor movement relative for games
    bool cursorMovementRelative = 0;

    sf::Color checkColor1 = sf::Color::Blue;

    sf::CircleShape dot(16,16);
    dot.setOrigin({dot.getRadius(),dot.getRadius()});    
    dot.setFillColor(sf::Color(255,255,255,128));
    //used to tell the cursor/average point to go
      float averageX = 0;
      float averageY = 0;    
      size_t detectedCount = 0;
      float sqrtDetectedArea = 0;
      //used in motion smoothing
      float averageXPrev = 0;
      float averageYPrev = 0;
      size_t detectedCountPrev = 0;
      bool smoothPointPosition = 0;
      float dampFactor = 0.9f;//this is "smoothing"//0.0-1.0//this means 90% towards the true calculated position for the frame, 10% toward the previous one 
      
      float smoothedX = 0.f;
      float smoothedY = 0.f; 
        
      float smoothedXPrev = 0.f;
      float smoothedYPrev = 0.f;   

 sf::Image prevImage;
 sf::Image currentImage;
    
 sf::Image tempImage;
{
        GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));

        if (sample) {
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            GstCaps *caps = gst_sample_get_caps(sample);
            GstStructure *s = gst_caps_get_structure(caps, 0);

            int width, height;
            gst_structure_get_int(s, "width", &width);
            gst_structure_get_int(s, "height", &height);
            
            GstMapInfo map;
            gst_buffer_map(buffer, &map, GST_MAP_READ);
           
            prevImage.resize({(unsigned)width,(unsigned)height},map.data);
            currentImage.resize({(unsigned)width,(unsigned)height});
            tempImage.resize({(unsigned)width,(unsigned)height});
       }
    
}
         const size_t imgWidth = prevImage.getSize().x;
         const size_t imgHeight = prevImage.getSize().y;

    sf::Texture outTexture;
     if (!outTexture.resize(prevImage.getSize())) std::cout << "failed to resize texture\n";
    sf::Sprite outSprite(outTexture);

 float imgScalar;
                
    sf::Font mainFont("/usr/share/fonts/X11/Type1/c0419bt_.pfb");
    sf::Text positionText(mainFont,"Test",40);


 while (window.isOpen())
    {
        if ((!forceWindowFocus)||window.hasFocus()){
        while (const std::optional event = window.pollEvent())
        {
            if (event->is<sf::Event::Closed>())
                window.close();
                
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::U)){

                fullscreenEnabled = !fullscreenEnabled;

                if (fullscreenEnabled) 
                    window.create(sf::VideoMode(window.getSize()), windowName,sf::State::Fullscreen);
                    else
                    window.create(sf::VideoMode(window.getSize()), windowName,sf::State::Windowed);
            }

        }

if (pickLock && sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Escape)){
    window.close();
}

        // Pull frame from appsink
        GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        
        if (sample) {
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            GstCaps *caps = gst_sample_get_caps(sample);
            GstStructure *s = gst_caps_get_structure(caps, 0);

            int width, height;
            gst_structure_get_int(s, "width", &width);
            gst_structure_get_int(s, "height", &height);

            GstMapInfo map;
            gst_buffer_map(buffer, &map, GST_MAP_READ);

            currentImage.resize({(unsigned)width,(unsigned)height},map.data);

            detectedCount = 0;
         
            cursorSensitivity = clampFloat(cursorSensitivity+(sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Add)-sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Subtract))*1.f,100.f,1000.f);
            colorThreshold = clampFloat(colorThreshold+(sf::Keyboard::isKeyPressed(sf::Keyboard::Key::M)-sf::Keyboard::isKeyPressed(sf::Keyboard::Key::N))*0.01f,0.f,1.f);
            luminanceThreshold = clampFloat(luminanceThreshold+(sf::Keyboard::isKeyPressed(sf::Keyboard::Key::K)-sf::Keyboard::isKeyPressed(sf::Keyboard::Key::J))*0.01f,0.f,1.f);

            minDiameterPercentThreshold = clampFloat(minDiameterPercentThreshold+(sf::Keyboard::isKeyPressed(sf::Keyboard::Key::O)-sf::Keyboard::isKeyPressed(sf::Keyboard::Key::I))*0.01f,0.01f,1.f);


            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::L))
            {
                if (!pickLockKeyPrev)
                    {         
                        pickLock = !pickLock;
                        if (pickLock) window.setFramerateLimit(30); else window.setFramerateLimit(targetFramerate);
                    }
                pickLockKeyPrev = 1;
            }
            else
            {
                pickLockKeyPrev = 0;
            }    

            tempImage.resize({(unsigned)imgWidth,(unsigned)imgHeight},sf::Color(0,0,0,0));

            window.clear();//gets covered up anyway but clear it anyway

            unsigned int nMinDiameterPixels  = clampFloat(imgWidth*minDiameterPercentThreshold,1.f,imgWidth);
            unsigned int nMinAreaPixels = nMinDiameterPixels*nMinDiameterPixels*quarterPi;

if ((pickLock))
{

            
            averageX = 0;
            averageY = 0;

            for (unsigned b = 0; b < imgHeight; b++)
                {
                    for (unsigned a = 0; a < imgWidth; a++)
                        {//color, luminance
                            if (getColorRatiosMatch(currentImage.getPixel({a,b}),checkColor1,colorThreshold,luminanceThreshold) || 0)
                                {//0.08//1.2 threshold FOR MATCHING *PERFECT COLOR* (green, red, blue, magenta, cyan, yellow)//max is 3.0f I believe
                                //   if ((currentImage.getPixel({a,b}).r-currentImage.getPixel({a,b}).r) > noiseThreshold || 0){
                                    if (((prevImage.getPixel({a,b}).r-currentImage.getPixel({a,b}).r) > noiseThreshold) || 1)
                                    {
                                        tempImage.setPixel({a,b},sf::Color::Red);
                                        averageX += a;
                                        averageY += b;
                                        detectedCount++;
                                    }
                    
                                }
                        }
                }      
                    
                 if (detectedCount){
                 averageX = averageX/detectedCount;
                 averageY = averageY/detectedCount;
} else {averageX = averageXPrev; averageY = averageYPrev;}
                
                float xdist = averageX-averageXPrev;
                float ydist = averageY-averageYPrev;
                float sdist = 1.f;
                if ((xdist!=0) || (ydist!=0))
                    sdist = sqrt(xdist*xdist+ydist*ydist);
                
                if (sdist<1.f) sdist = 1.f;

             
                bool minimumCountMade = (detectedCount)&&(detectedCount > minimumDetectedPixels);
                                                                                 //crucially, I made it so if the minimum count is not made, the cursor finds a new point no matter where it is. If you don't do this it gets stuck
           if (((!jitterProtection)||(minimumCountMade)||(sdist<localDetectOverrideLimit)) && ((!rangeProtection)||(!minimumCountMade)||(sdist<localDetectOverrideLimit))){
         
           //do nothing here because the averaging happens before now like it should
}
else{
 averageX = averageXPrev;  
 averageY = averageYPrev;   
}
            
            sqrtDetectedArea = clampFloat(sqrt(detectedCount),1,99999999);

if (sf::Mouse::isButtonPressed(sf::Mouse::Button::Left)&&window.hasFocus()){
               checkColor1 = currentImage.getPixel((sf::Vector2u)(((sf::Vector2f)sf::Mouse::getPosition()).componentWiseDiv(outSprite.getScale())));
}}
else
{
            for (unsigned b = 0; b < imgHeight; b+=nMinDiameterPixels)
                {
                    for (unsigned a = 0; a < imgWidth; a+=nMinDiameterPixels)
                        {
                            tempImage.setPixel({a,b},sf::Color::Green);
                            //std::cout << a << ", " << b << std::endl;
                            if (getColorRatiosMatch(currentImage.getPixel({a,b}),checkColor1,colorThreshold,luminanceThreshold) || 0)
                                {
                                    if (((prevImage.getPixel({a,b}).r-currentImage.getPixel({a,b}).r) > noiseThreshold) || 1)
                                    {
                                        //tempImage.setPixel({a,b},sf::Color::Green);
                                        long int leftMostX;
                                        for (leftMostX = a-1; leftMostX > 0; leftMostX--)
                                        {
                                            tempImage.setPixel({(unsigned)leftMostX,b},sf::Color::Blue);

                                            if (!getColorRatiosMatch(currentImage.getPixel({(unsigned)leftMostX,b}),checkColor1,colorThreshold,luminanceThreshold))
                                                {
                                                    leftMostX++;
                                                    break;
                                                }
                                        }
                                                                                         
                                        long int rightMostX;
                                        for (rightMostX = a+1; rightMostX < imgWidth; rightMostX++)
                                        {
                                            tempImage.setPixel({(unsigned)rightMostX,b},sf::Color::Blue);
                                            if (!getColorRatiosMatch(currentImage.getPixel({(unsigned)rightMostX,b}),checkColor1,colorThreshold,luminanceThreshold))
                                                {
                                                    rightMostX--;
                                                    break;
                                                }
                                        }
                                        
                                        long int initialMidPointX = (leftMostX + rightMostX)/2;
                                        
                                        long int topMostY;
                                        for (topMostY = b-1; topMostY > 0; topMostY--)
                                        {
                                            tempImage.setPixel({(unsigned)initialMidPointX,(unsigned)topMostY},sf::Color::Red);
                                            if (!getColorRatiosMatch(currentImage.getPixel({(unsigned)initialMidPointX,(unsigned)topMostY}),checkColor1,colorThreshold,luminanceThreshold))
                                                {
                                                    topMostY++;
                                                    break;
                                                }
                                        }

                                        long int bottomMostY;
                                        for (bottomMostY = b+1; bottomMostY < imgHeight; bottomMostY++)
                                        {
                                            tempImage.setPixel({(unsigned)initialMidPointX,(unsigned)bottomMostY},sf::Color::Red);
                                            if (!getColorRatiosMatch(currentImage.getPixel({(unsigned)initialMidPointX,(unsigned)bottomMostY}),checkColor1,colorThreshold,luminanceThreshold))
                                                {
                                                    bottomMostY--;
                                                    break;
                                                }
                                        }

                                        long int YSpanLength = bottomMostY - topMostY;
                                        long int finalMidPointY = (topMostY + bottomMostY)/2;
    
                            
                                        for (leftMostX = a-1; leftMostX > 0; leftMostX--)
                                        {
                                            tempImage.setPixel({(unsigned)leftMostX,(unsigned)finalMidPointY},sf::Color::Red);
                                            if (!getColorRatiosMatch(currentImage.getPixel({(unsigned)leftMostX,(unsigned)finalMidPointY}),checkColor1,colorThreshold,luminanceThreshold))
                                                {
                                                    leftMostX++;
                                                    break;
                                                }
                                        }
                                                                                         
                                        
                                        for (rightMostX = a+1; rightMostX < imgWidth; rightMostX++)
                                        {
                                            tempImage.setPixel({(unsigned)rightMostX,(unsigned)finalMidPointY},sf::Color::Red);
                                            if (!getColorRatiosMatch(currentImage.getPixel({(unsigned)rightMostX,(unsigned)finalMidPointY}),checkColor1,colorThreshold,luminanceThreshold))
                                                {
                                                    rightMostX--;
                                                    break;
                                                } 
                                        }                                        

                                        long int XSpanLength = rightMostX - leftMostX;
                                        long int finalMidPointX = (leftMostX + rightMostX)/2;

                                        float sqrtTrackerArea = sqrt(clampFloat(((XSpanLength * YSpanLength)*quarterPi),1,999999));//keeps circular area to match initial detection numbers
                                        
                                        if (sqrtTrackerArea>=sqrt(nMinAreaPixels))
                                            {
                                                sqrtDetectedArea = sqrtTrackerArea;
                                                tempImage.setPixel({(unsigned)finalMidPointX,(unsigned)finalMidPointY},sf::Color::Yellow);
                                                averageX = finalMidPointX;
                                                averageY = finalMidPointY;
                                                a = imgWidth;
                                                b = imgHeight;
                                            }
                                        
                                    }
                    
                                }
                        }
                } 
        averageXPrev = averageX;
        averageYPrev = averageY;
        prevImage = currentImage;

                std::string command = "xdotool mousemove";
            if (cursorMovementRelative){
        command += "_relative -- ";

        const float multiplier = 200.f;

        const float xMove = (smoothedX-smoothedXPrev)/sqrtDetectedArea; 
        const float yMove = (smoothedY-smoothedYPrev)/sqrtDetectedArea; 
        command += std::to_string(-xMove*multiplier) + " " + std::to_string(yMove*multiplier);//keep y the same
        std::system(command.c_str());
}
else{
command+= " --";
command+=" ";
command+= std::to_string((float)(imgWidth/2.f+(imgWidth/2.f-smoothedX)*cursorSensitivity/sqrtDetectedArea)*outSprite.getScale().x) + " " + std::to_string((float)(imgHeight/2.f-(imgHeight/2.f-smoothedY)*cursorSensitivity/sqrtDetectedArea)*outSprite.getScale().y);
    std::system(command.c_str());
}                   
}


if (smoothPointPosition){
        //smoothly shift the point towards its next target
        //dependent on dampFactor. If 1.0, still instant snap, if 0.0, it doesn't move
        smoothedX += ((float)averageX-smoothedX)*dampFactor;
        smoothedY += ((float)averageY-smoothedY)*dampFactor;
}
else{
        //not so smooth, directly snap the point
        smoothedX = averageX;
        smoothedY = averageY;
}

        averageXPrev = averageX;
        averageYPrev = averageY;

            detectedCountPrev = detectedCount;
            prevImage = currentImage;


    smoothedXPrev = smoothedX;
    smoothedYPrev = smoothedY;


            outTexture.update(currentImage);

            if (imgWidth<imgHeight) imgScalar = (float)window.getSize().x/imgWidth; else imgScalar = (float)window.getSize().y/imgHeight;

            outSprite.setScale({imgScalar,imgScalar});
            dot.setPosition(sf::Vector2f{smoothedX,smoothedY}.componentWiseMul(outSprite.getScale()));

            window.draw(outSprite);
            outTexture.update(tempImage);
            window.draw(outSprite);
            
        
            dot.setRadius(sqrtDetectedArea/4.f);//5.f
            dot.setOrigin({dot.getRadius(),dot.getRadius()});
            window.draw(dot);
            

            positionText.setCharacterSize(clampFloat(0.5f*(float)sqrtDetectedArea,15,100));
            positionText.setString("x:" + std::to_string((int)smoothedX) + ",y:" + std::to_string((int)smoothedY) + ",s:" + std::to_string((((float)nCalibrationPixelsArea/sqrtDetectedArea)*referenceDistanceInches)));
            positionText.setPosition(sf::Vector2f{smoothedX,smoothedY}.componentWiseMul(outSprite.getScale()));    
            window.draw(positionText);

            window.display();

            gst_buffer_unmap(buffer, &map);
            gst_sample_unref(sample);

        }
    }
}
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
};

 std::thread trackingThread(trackingLoop);
    trackingThread.join();
    return 0;

}




