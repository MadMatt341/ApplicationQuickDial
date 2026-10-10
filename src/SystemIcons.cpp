#include "SystemIcons.h"
#include <wrl/client.h>
#include <cmath>
#include <numbers>
#include <algorithm>

namespace quickdial {
void DrawSystemIcon(ID2D1Factory& factory, ID2D1RenderTarget& target, MenuItemId item,
                    D2D1_POINT_2F center, ID2D1Brush& brush, unsigned dpi) {
  const float scale = static_cast<float>(dpi) / 96.0f;
  const float stroke = std::max(1.0f, std::round(1.75f * scale)) / scale;
  center.x = (std::round(center.x * scale - stroke * scale / 2) + stroke * scale / 2) / scale;
  center.y = (std::round(center.y * scale - stroke * scale / 2) + stroke * scale / 2) / scale;
  const auto point = [center](float x, float y) { return D2D1::Point2F(center.x + x, center.y + y); };
  const auto line = [&](float x1, float y1, float x2, float y2) { target.DrawLine(point(x1,y1), point(x2,y2), &brush, stroke); };
  if(item==MenuItemId::Appearance) {
    for(float y : {-7.0f,0.0f,7.0f}) {line(-10,y,10,y); target.DrawEllipse(D2D1::Ellipse(point(y==0 ? 5.0f : -4.0f,y),2.5f,2.5f),&brush,stroke);}
    return;
  }
  if (item == MenuItemId::System) {
    target.DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(center.x-11,center.y-9,center.x+11,center.y+5),2,2), &brush,stroke);
    line(0,5,0,10); line(-6,10,6,10);
    return;
  }
  if (item == MenuItemId::SignOut) {
    target.DrawRectangle(D2D1::RectF(center.x-10,center.y-10,center.x+1,center.y+10), &brush,stroke);
    line(-3,0,11,0); line(6,-5,11,0); line(11,0,6,5);
    return;
  }
  Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
  Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(factory.CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) return;
  constexpr float radius = 9.0f;
  const float start = item == MenuItemId::ShutDown ? -45.0f : -65.0f;
  const float finish = item == MenuItemId::ShutDown ? -135.0f : -120.0f;
  const auto circle = [&](float degrees) {
    const float angle = degrees * std::numbers::pi_v<float> / 180.0f;
    return point(radius*std::cos(angle), radius*std::sin(angle));
  };
  sink->BeginFigure(circle(start), D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddArc(D2D1::ArcSegment(circle(finish), D2D1::SizeF(radius,radius), 0,
      D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  if (FAILED(sink->Close())) return;
  target.DrawGeometry(geometry.Get(), &brush, stroke);
  if (item == MenuItemId::ShutDown) { line(0,-12,0,-1); return; }
  // Arrowhead follows the tangent of the open circular arc, rather than covering a full circle.
  const auto end = circle(finish);
  const float angle = finish * std::numbers::pi_v<float> / 180.0f;
  const float backX = std::sin(angle), backY = -std::cos(angle);
  for (const float side : {-1.0f, 1.0f})
    target.DrawLine(end, D2D1::Point2F(end.x+5*(backX*0.82f-side*backY*0.57f),
        end.y+5*(backY*0.82f+side*backX*0.57f)), &brush, stroke);
}
}
