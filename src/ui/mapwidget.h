#ifndef _UI_MAPWIDGET_H
#define _UI_MAPWIDGET_H

#include "../uilib/image.h"
#include <map>
#include <list>

namespace Ui {

class MapWidget : public Image {
public:
    MapWidget(int x, int y, int w, int h, const char* filename);
    MapWidget(int x, int y, int w, int h, const void* data, size_t len);
    
    struct Point {
        int x=0;
        int y=0;
        int size=0;
        int borderThickness=0;
    };
    
    struct Location {
        std::list<Point> pos;
        int state=1;
    };
    
    // TODO: enum location state
    void addLocation(const std::string& name, int x, int y, int size, int borderThickness, int state=1);
    void setLocationState(const std::string& name, int state);
    
    // FIXME: this does not work if name is not unique
    Signal<const std::string&,int,int> onLocationHover; // FIXME: we should provide absolute AND relative mouse position through the Event stack
    
    virtual void render(Renderer renderer, int offX, int offY);
    int getAbsLeft() const { return _absX; } // FIXME: this is not really a good solution
    int getAbsTop() const { return _absY; }

    void setHideClearedLocations(bool hide) { _hideClearedLocations = hide; printf("hideCleared: %s\n", hide?"true":"false"); }
    void setHideUnreachableLocations(bool hide) { _hideUnreachableLocations = hide; printf("hideUnreachable: %s\n", hide?"true":"false"); }

protected:
    int _absX=0;
    int _absY=0;
    std::map<std::string, Location> _locations;
    std::string _locationHover; // TODO; store iterator instead of string?

    bool _hideClearedLocations = false;
    bool _hideUnreachableLocations = false;

private:
    void connectSignals();
    void calculateSizes(int left, int top, int& srcw, int& srch, int& dstx, int& dsty, int& dstw, int& dsth);
};

} // namespace Ui

#endif
