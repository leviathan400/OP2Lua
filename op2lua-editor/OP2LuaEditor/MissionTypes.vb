' MissionTypes.vb
'
' Data types for the OP2Lua region/unit editing layer. Kept as top-level types in their OWN file
' (NOT nested inside the fMain Form partial) so the WinForms designer never inspects them when it
' loads fMain - nested public types in a Form partial make the designer fail to resolve the base class.

Public Enum RegionKind
    Rectangle
    Point
End Enum

' A named region: a rectangle (x1,y1)-(x2,y2) or a single point. 1-based tile coords (status-bar /
' placement.lua space).
Public Class MissionRegion
    Public Name As String
    Public Kind As RegionKind
    Public X1 As Integer
    Public Y1 As Integer
    Public X2 As Integer
    Public Y2 As Integer

    Public Overrides Function ToString() As String
        If Kind = RegionKind.Point Then
            Return Name & "  (" & X1 & "," & Y1 & ")"
        End If
        Return Name & "  (" & X1 & "," & Y1 & ")-(" & X2 & "," & Y2 & ")"
    End Function
End Class

' A unit parsed from placement.lua, drawn as a labelled placeholder (no sprites) for context.
Public Class MissionUnit
    Public UnitType As String
    Public Player As Integer
    Public X As Integer        ' 1-based tile coords
    Public Y As Integer
    Public Name As String
    Public Weapon As String    ' optional, e.g. "Laser" ("" = none)
    Public Cargo As String     ' optional Cargo Truck load
    Public Amount As Integer   ' optional cargo amount

    Public Overrides Function ToString() As String
        Dim w As String = If(String.IsNullOrEmpty(Weapon), "", " " & Weapon)
        Return UnitType & w & "  p" & Player & " (" & X & "," & Y & ")"
    End Function
End Class

' What an in-progress mouse drag is doing to a region.
Friend Enum EditAction
    None
    Drawing      ' rubber-banding a new region
    Moving       ' dragging an existing region's body
    NW           ' dragging a corner/edge handle (resize)
    N
    NE
    E
    SE
    S
    SW
    W
End Enum
