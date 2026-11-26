' fMain.Regions.vb
'
' The OP2Lua mission layer of the editor, on top of the map viewer in fMain.vb (a partial class of the same form):
'   - the Mission, Regions and Units menus and the side panel's regions / units lists;
'   - opening a mission (placement.lua + its map), New Mission, Mission Properties, Save, and Test in OP2 (F5), which
'     copies the mission into the game and starts it through op2launcher;
'   - drawing the placement over the map: regions, units, beacons / walls / tubes / markers and dozed borders;
'   - the status bar's mission and OP2LuaCore labels.
' Reading and rewriting placement.lua text is in PlacementText.vb; the data types are in MissionTypes.vb.
'
' Coordinates are 1-based tile coordinates, the numbers the game's status bar shows and placement.lua holds; the map
' bitmap's tiles are 0-based (TileToLua / LuaToTile).
'
' What fMain.vb calls here:
'   fMain_Load           -> SetupRegionsUI(), UpdateCoreLabel(), OpenMissionFile(path) for a .lua on the command line
'   Settings             -> UpdateCoreLabel()
'   MapPanel_Paint       -> DrawRegions(e.Graphics) (regions, then dozed borders, map objects and units)
'   MapPanel_Mouse*      -> RegionMouseDown/Move/Up, UnitMouseDown/Move/Up in their modes; MouseToTileX/Y
'   UpdatePropertyControls -> TileContentsText(x, y) for the side panel
'   Close Map            -> ClearMissionData(), UpdateMissionLabel()
'   Image export         -> RenderOverlayMapPixels(g)
'   a .lua dropped on the map -> OpenMissionFile(path)

Imports System.Drawing
Imports System.IO
Imports System.Text
Imports System.Text.RegularExpressions
Imports System.Windows.Forms

Partial Public Class fMain

    ' The data types (MissionRegion, MissionUnit, ...) are in MissionTypes.vb, outside the form, so the Form designer can
    ' load fMain.

    ' ---- state ----
    Private regionList As New List(Of MissionRegion)
    Private unitsBlock As New PlacementText.UnitsBlock()        ' the units table as read from placement.lua (+ edits)
    Private mapObjects As New List(Of MissionMapObject)          ' beacons, walls/tubes and markers (shown, not edited)
    Private showUnits As Boolean = True
    Private showMapObjects As Boolean = True

    Private ReadOnly Property unitList As List(Of MissionUnit)
        Get
            Return unitsBlock.Units
        End Get
    End Property
    Private regionMode As Boolean = False
    Private regionDrawing As Boolean = False
    Private regionStartTX As Integer, regionStartTY As Integer       ' 0-based tile during drag
    Private regionCurTX As Integer, regionCurTY As Integer
    Private lstRegions As ListBox = Nothing
    Private lblRegionsHeader As Label = Nothing
    Private lblUnitsHeader As Label = Nothing
    Private listsSplit As SplitContainer = Nothing   ' regions above, units below, under the tile info
    Private Const InfoHeight As Integer = 240        ' side-panel height kept for the tile info above the lists
    Private mnuRegionMode As ToolStripMenuItem = Nothing
    Private mnuShowUnits As ToolStripMenuItem = Nothing
    Private mnuShowMapObjects As ToolStripMenuItem = Nothing
    Private regionsMenuTop As ToolStripMenuItem = Nothing    ' top-level "Regions" menu (disabled until a map is loaded)
    Private unitsMenuTop As ToolStripMenuItem = Nothing      ' top-level "Units" menu (disabled until a map is loaded)
    Private missionMenuTop As ToolStripMenuItem = Nothing    ' top-level "Mission" menu (always enabled)
    Private mnuMissionProps As ToolStripMenuItem = Nothing   ' "Properties..." (enabled once a mission is open)
    Private mnuSaveMission As ToolStripMenuItem = Nothing     ' "Save Mission" (enabled once a map is loaded)
    Private mnuTestInOp2 As ToolStripMenuItem = Nothing       ' "Test in OP2" (enabled once a mission is open)

    ' ---- unit editing state ----
    Private unitMode As Boolean = False
    Private currentUnitType As String = "Lynx"
    Private currentUnitPlayer As Integer = 1
    Private currentUnitWeapon As String = ""          ' "" = none
    Private lstUnits As ListBox = Nothing
    Private mnuUnitMode As ToolStripMenuItem = Nothing
    Private unitDragging As Boolean = False
    Private unitDragIndex As Integer = -1

    ' The unit and weapon names OP2Lua's placement takes (src/Placement.cpp MapIdFromName).
    Private ReadOnly vehicleTypeChoices As String() = New String() {
        "Lynx", "Panther", "Tiger", "Scorpion", "ConVec", "CargoTruck", "RoboMiner", "RoboSurveyor",
        "RoboDozer", "GeoCon", "Earthworker", "Scout", "Spider", "RepairVehicle", "EvacuationTransport"}
    Private ReadOnly structureTypeChoices As String() = New String() {
        "CommandCenter", "Tokamak", "MHDGenerator", "SolarPowerArray", "GeothermalPlant",
        "CommonOreMine", "RareOreMine", "MagmaWell", "CommonOreSmelter", "RareOreSmelter", "CommonStorage", "RareStorage",
        "StructureFactory", "VehicleFactory", "ArachnidFactory", "ConsumerFactory", "Garage", "Spaceport",
        "BasicLab", "StandardLab", "AdvancedLab", "Observatory",
        "Residence", "ReinforcedResidence", "AdvancedResidence", "Agridome", "Nursery", "University", "MedicalCenter",
        "Forum", "RecreationFacility", "DIRT", "RobotCommand", "TradeCenter", "MeteorDefense", "GORF",
        "GuardPost", "LightTower", "Tube", "Wall", "LavaWall", "MicrobeWall"}
    ' EnergyCannon is the Scorpion's weapon.
    Private ReadOnly unitWeaponChoices As String() = New String() {
        "(none)", "Laser", "Microwave", "RailGun", "RPG", "EMP", "ESG", "Stickyfoam", "ThorsHammer",
        "AcidCloud", "Starflare", "Supernova", "EnergyCannon"}

    ' ---- edit state ----
    Private editOp As EditAction = EditAction.None
    Private editIndex As Integer = -1
    Private moveStartTX As Integer, moveStartTY As Integer
    Private origX1 As Integer, origY1 As Integer, origX2 As Integer, origY2 As Integer
    Private Const HandlePx As Integer = 8                            ' resize-handle hit/draw size

    Private ReadOnly regionColors As Color() = New Color() {
        Color.Cyan, Color.Yellow, Color.Lime, Color.Magenta, Color.Orange,
        Color.DeepSkyBlue, Color.HotPink, Color.GreenYellow, Color.Gold, Color.Aqua}

    ' ===== coordinate helpers (single place to change if a map padding offset is ever needed) =====
    Private Function TileToLua(t As Integer) As Integer
        Return t + 1
    End Function
    Private Function LuaToTile(l As Integer) As Integer
        Return l - 1
    End Function
    Private Function MouseToTileX(px As Integer) As Integer
        Return CInt(Math.Floor((px - offsetX) / zoomLevel / 32))
    End Function
    Private Function MouseToTileY(py As Integer) As Integer
        Return CInt(Math.Floor((py - offsetY) / zoomLevel / 32))
    End Function
    ' Clamp a 0-based tile coordinate to the loaded map so regions can't be drawn off the edges.
    Private Function ClampTileX(tx As Integer) As Integer
        If currentMap Is Nothing Then Return tx
        Return Math.Max(0, Math.Min(tx, currentMap.WidthInTiles() - 1))
    End Function
    Private Function ClampTileY(ty As Integer) As Integer
        If currentMap Is Nothing Then Return ty
        Return Math.Max(0, Math.Min(ty, currentMap.HeightInTiles() - 1))
    End Function

    ' ===== UI setup (called from fMain_Load, after InitializeUI) =====
    Public Sub SetupRegionsUI()
        ' --- "Mission" top menu (inserted before Help) - New Mission (always) + Properties (needs one open) ---
        Dim missionMenu As New ToolStripMenuItem("Mission")
        missionMenuTop = missionMenu
        Dim mNew As New ToolStripMenuItem("New Mission...")
        AddHandler mNew.Click, AddressOf NewMission_Click
        missionMenu.DropDownItems.Add(mNew)
        missionMenu.DropDownItems.Add(New ToolStripSeparator())
        mnuSaveMission = New ToolStripMenuItem("Save Mission (regions + units)")
        mnuSaveMission.ShortcutKeys = Keys.Control Or Keys.S
        mnuSaveMission.Enabled = False        ' enabled once a map is loaded
        AddHandler mnuSaveMission.Click, AddressOf SaveMission_Click
        missionMenu.DropDownItems.Add(mnuSaveMission)
        missionMenu.DropDownItems.Add(New ToolStripSeparator())
        mnuMissionProps = New ToolStripMenuItem("Properties...")
        mnuMissionProps.Enabled = False      ' enabled once a mission (placement.lua) is open
        AddHandler mnuMissionProps.Click, AddressOf MissionProps_Click
        missionMenu.DropDownItems.Add(mnuMissionProps)
        missionMenu.DropDownItems.Add(New ToolStripSeparator())
        mnuTestInOp2 = New ToolStripMenuItem("Test in OP2")
        mnuTestInOp2.ShortcutKeys = Keys.F5
        mnuTestInOp2.Enabled = False         ' enabled once a mission (placement.lua) is open
        AddHandler mnuTestInOp2.Click, AddressOf TestInOp2_Click
        missionMenu.DropDownItems.Add(mnuTestInOp2)
        If MainMenuStrip IsNot Nothing Then
            Dim insertAtM As Integer = Math.Max(0, MainMenuStrip.Items.Count - 1)   ' before Help
            MainMenuStrip.Items.Insert(insertAtM, missionMenu)
        End If

        ' --- "Regions" top menu (inserted before Help) ---
        Dim regionsMenu As New ToolStripMenuItem("Regions")
        regionsMenu.Enabled = False     ' enabled once a map is loaded
        regionsMenuTop = regionsMenu

        mnuRegionMode = New ToolStripMenuItem("Region Draw Mode")
        mnuRegionMode.CheckOnClick = True
        AddHandler mnuRegionMode.Click, AddressOf RegionMode_Click
        regionsMenu.DropDownItems.Add(mnuRegionMode)

        mnuShowUnits = New ToolStripMenuItem("Show Units (from placement.lua)")
        mnuShowUnits.CheckOnClick = True
        mnuShowUnits.Checked = showUnits
        AddHandler mnuShowUnits.Click, AddressOf ShowUnits_Click
        regionsMenu.DropDownItems.Add(mnuShowUnits)

        mnuShowMapObjects = New ToolStripMenuItem("Show Beacons, Walls and Markers")
        mnuShowMapObjects.CheckOnClick = True
        mnuShowMapObjects.Checked = showMapObjects
        AddHandler mnuShowMapObjects.Click, Sub(s, ev)
                                                showMapObjects = mnuShowMapObjects.Checked
                                                InvalidateMap()
                                            End Sub
        regionsMenu.DropDownItems.Add(mnuShowMapObjects)
        regionsMenu.DropDownItems.Add(New ToolStripSeparator())

        Dim mLoad As New ToolStripMenuItem("Load Regions from placement.lua...")
        AddHandler mLoad.Click, AddressOf LoadRegions_Click
        regionsMenu.DropDownItems.Add(mLoad)

        Dim mSave As New ToolStripMenuItem("Save Regions to placement.lua...")
        AddHandler mSave.Click, AddressOf SaveRegions_Click
        regionsMenu.DropDownItems.Add(mSave)
        regionsMenu.DropDownItems.Add(New ToolStripSeparator())

        Dim mRename As New ToolStripMenuItem("Rename Selected Region")
        AddHandler mRename.Click, AddressOf RenameRegion_Click
        regionsMenu.DropDownItems.Add(mRename)

        Dim mDelete As New ToolStripMenuItem("Delete Selected Region")
        AddHandler mDelete.Click, AddressOf DeleteRegion_Click
        regionsMenu.DropDownItems.Add(mDelete)

        Dim mClear As New ToolStripMenuItem("Clear All Regions")
        AddHandler mClear.Click, AddressOf ClearRegions_Click
        regionsMenu.DropDownItems.Add(mClear)

        If MainMenuStrip IsNot Nothing Then
            Dim insertAt As Integer = Math.Max(0, MainMenuStrip.Items.Count - 1)   ' before Help
            MainMenuStrip.Items.Insert(insertAt, regionsMenu)
        End If

        ' --- regions and units lists: a split filling the side panel below the tile info (drag the bar between) ---
        Dim props As Control = GetPropertiesPanel()
        listsSplit = New SplitContainer() With {.Orientation = Orientation.Horizontal, .Dock = DockStyle.Bottom,
                                                .Height = 420, .SplitterWidth = 6, .BorderStyle = BorderStyle.None}
        props.Controls.Add(listsSplit)
        AddHandler props.Resize, Sub(sender2, e2) SizeListsSplit(props)

        lblRegionsHeader = ListHeader("Regions")
        lstRegions = New ListBox()
        lstRegions.Name = "lstRegions"
        lstRegions.Dock = DockStyle.Fill
        lstRegions.IntegralHeight = False
        lstRegions.Font = ListFont()
        lstRegions.HorizontalScrollbar = True
        AddHandler lstRegions.SelectedIndexChanged, AddressOf RegionSelection_Changed
        AddHandler lstRegions.DoubleClick, AddressOf RenameRegion_Click
        AddHandler lstRegions.MouseDown, AddressOf RegionList_MouseDown

        ' right-click context menu on the regions list
        Dim ctx As New ContextMenuStrip()
        Dim ctxRename As New ToolStripMenuItem("Rename Region")
        AddHandler ctxRename.Click, AddressOf RenameRegion_Click
        Dim ctxDelete As New ToolStripMenuItem("Delete Region")
        AddHandler ctxDelete.Click, AddressOf DeleteRegion_Click
        ctx.Items.Add(ctxRename)
        ctx.Items.Add(ctxDelete)
        lstRegions.ContextMenuStrip = ctx

        listsSplit.Panel1.Controls.Add(lstRegions)
        listsSplit.Panel1.Controls.Add(lblRegionsHeader)
        lstRegions.BringToFront()                    ' the Fill control docks last, below the header
        RefreshRegionList()

        SetupUnitsUI()
    End Sub

    ' Right-click selects the item under the cursor first, so the context action targets it.
    Private Sub RegionList_MouseDown(sender As Object, e As MouseEventArgs)
        If e.Button = MouseButtons.Right AndAlso lstRegions IsNot Nothing Then
            Dim idx As Integer = lstRegions.IndexFromPoint(e.Location)
            If idx >= 0 Then lstRegions.SelectedIndex = idx
        End If
    End Sub

    ' Find the right-docked properties panel; fall back to the form.
    Private Function GetPropertiesPanel() As Control
        For Each ctrl As Control In Me.Controls
            If TypeOf ctrl Is Panel AndAlso ctrl.Dock = DockStyle.Right Then
                Return ctrl
            End If
        Next
        Return Me
    End Function

    Private Sub RegionMode_Click(sender As Object, e As EventArgs)
        regionMode = mnuRegionMode.Checked
        If regionMode AndAlso mnuUnitMode IsNot Nothing Then
            mnuUnitMode.Checked = False : unitMode = False     ' modes are mutually exclusive
        End If
        UpdateMapCursor()
    End Sub

    Private Sub UpdateMapCursor()
        Dim panel As Control = FindControl("pnlMap")
        If panel IsNot Nothing Then
            panel.Cursor = If(regionMode OrElse unitMode, Cursors.Cross, Cursors.Default)
        End If
    End Sub

    Private Sub ShowUnits_Click(sender As Object, e As EventArgs)
        showUnits = mnuShowUnits.Checked
        Dim panel As Control = FindControl("pnlMap")
        If panel IsNot Nothing Then panel.Invalidate()
    End Sub

    Private Sub RegionSelection_Changed(sender As Object, e As EventArgs)
        Dim panel As Control = FindControl("pnlMap")
        If panel IsNot Nothing Then panel.Invalidate()
    End Sub

    ' ===== mouse interaction (called from the map panel handlers when regionMode is on) =====
    ' In region mode a left press either: grabs a resize handle of the selected region, grabs the
    ' body of a region to move it, or (on empty ground) starts drawing a new region.
    ' Returns True if it consumed the event (so the normal tile-select is skipped).
    Public Function RegionMouseDown(e As MouseEventArgs) As Boolean
        If Not regionMode OrElse currentMap Is Nothing Then Return False
        If e.Button <> MouseButtons.Left Then Return False    ' right button still pans

        Dim tx As Integer = MouseToTileX(e.X)
        Dim ty As Integer = MouseToTileY(e.Y)

        ' 1) resize handle of the currently selected (rectangle) region
        Dim sel As Integer = If(lstRegions IsNot Nothing, lstRegions.SelectedIndex, -1)
        If sel >= 0 AndAlso sel < regionList.Count Then
            Dim h As EditAction = HitHandle(regionList(sel), e.X, e.Y)
            If h <> EditAction.None Then
                editOp = h
                editIndex = sel
                Return True
            End If
        End If

        ' 2) clicking inside an existing region -> select it and move it (topmost wins)
        For i As Integer = regionList.Count - 1 To 0 Step -1
            If TileInRegionLua(regionList(i), tx, ty) Then
                If lstRegions IsNot Nothing Then lstRegions.SelectedIndex = i
                editOp = EditAction.Moving
                editIndex = i
                moveStartTX = tx : moveStartTY = ty
                origX1 = regionList(i).X1 : origY1 = regionList(i).Y1
                origX2 = regionList(i).X2 : origY2 = regionList(i).Y2
                InvalidateMap()
                Return True
            End If
        Next

        ' 3) empty ground -> start drawing a new region
        If tx < 0 OrElse ty < 0 OrElse tx >= currentMap.WidthInTiles() OrElse ty >= currentMap.HeightInTiles() Then
            Return True
        End If
        editOp = EditAction.Drawing
        regionDrawing = True
        regionStartTX = tx : regionStartTY = ty
        regionCurTX = tx : regionCurTY = ty
        Return True
    End Function

    ' In region mode: rubber-band the region being drawn, or move / resize the one grabbed.
    Public Sub RegionMouseMove(e As MouseEventArgs)
        If Not regionMode OrElse editOp = EditAction.None Then Return
        Dim tx As Integer = ClampTileX(MouseToTileX(e.X))
        Dim ty As Integer = ClampTileY(MouseToTileY(e.Y))

        Select Case editOp
            Case EditAction.Drawing
                regionCurTX = tx : regionCurTY = ty
            Case EditAction.Moving
                ' Shift the whole rect, clamped so it stays fully inside the map (lua coords are 1..W / 1..H).
                Dim r As MissionRegion = regionList(editIndex)
                Dim w As Integer = origX2 - origX1
                Dim h As Integer = origY2 - origY1
                Dim nx1 As Integer = origX1 + (tx - moveStartTX)
                Dim ny1 As Integer = origY1 + (ty - moveStartTY)
                nx1 = Math.Max(1, Math.Min(nx1, currentMap.WidthInTiles() - w))
                ny1 = Math.Max(1, Math.Min(ny1, currentMap.HeightInTiles() - h))
                r.X1 = nx1 : r.Y1 = ny1
                r.X2 = nx1 + w : r.Y2 = ny1 + h
            Case Else   ' one of the resize handles (tx,ty already clamped to the map)
                ResizeEdge(regionList(editIndex), editOp, tx, ty)
        End Select
        InvalidateMap()
    End Sub

    ' In region mode: finish the drawing (a new region, named by the user) or the move / resize.
    Public Sub RegionMouseUp(e As MouseEventArgs)
        If Not regionMode OrElse editOp = EditAction.None Then Return
        If e.Button <> MouseButtons.Left Then Return

        Dim act As EditAction = editOp
        editOp = EditAction.None
        regionDrawing = False

        If act = EditAction.Drawing Then
            FinishDrawing()
        Else
            If editIndex >= 0 AndAlso editIndex < regionList.Count Then NormalizeRegion(regionList(editIndex))
            RefreshRegionList()
        End If
        InvalidateMap()
    End Sub

    ' Create the region from the rubber-band drag (drag = rectangle, single tile = point).
    Private Sub FinishDrawing()
        Dim t0x As Integer = Math.Min(regionStartTX, regionCurTX)
        Dim t0y As Integer = Math.Min(regionStartTY, regionCurTY)
        Dim t1x As Integer = Math.Max(regionStartTX, regionCurTX)
        Dim t1y As Integer = Math.Max(regionStartTY, regionCurTY)

        Dim defName As String = "region" & (regionList.Count + 1)
        Dim name As String = InputBox("Region name:", "New Region", defName)
        If String.IsNullOrWhiteSpace(name) Then Return
        name = name.Trim()

        Dim r As New MissionRegion()
        r.Name = name
        If t0x = t1x AndAlso t0y = t1y Then
            r.Kind = RegionKind.Point
            r.X1 = TileToLua(t0x) : r.Y1 = TileToLua(t0y)
            r.X2 = r.X1 : r.Y2 = r.Y1
        Else
            r.Kind = RegionKind.Rectangle
            r.X1 = TileToLua(t0x) : r.Y1 = TileToLua(t0y)
            r.X2 = TileToLua(t1x) : r.Y2 = TileToLua(t1y)
        End If
        regionList.Add(r)
        RefreshRegionList()
        If lstRegions IsNot Nothing Then lstRegions.SelectedIndex = regionList.Count - 1
    End Sub

    ' Apply a resize-handle drag (tx,ty are 0-based tiles) to the given edge(s) of region r.
    Private Sub ResizeEdge(r As MissionRegion, a As EditAction, tx As Integer, ty As Integer)
        Dim lx As Integer = TileToLua(tx)
        Dim ly As Integer = TileToLua(ty)
        Select Case a
            Case EditAction.NW : r.X1 = lx : r.Y1 = ly
            Case EditAction.N : r.Y1 = ly
            Case EditAction.NE : r.X2 = lx : r.Y1 = ly
            Case EditAction.E : r.X2 = lx
            Case EditAction.SE : r.X2 = lx : r.Y2 = ly
            Case EditAction.S : r.Y2 = ly
            Case EditAction.SW : r.X1 = lx : r.Y2 = ly
            Case EditAction.W : r.X1 = lx
        End Select
    End Sub

    ' Ensure X1<=X2 and Y1<=Y2 after a resize that crossed an edge.
    Private Sub NormalizeRegion(r As MissionRegion)
        If r.X1 > r.X2 Then Dim t = r.X1 : r.X1 = r.X2 : r.X2 = t
        If r.Y1 > r.Y2 Then Dim t = r.Y1 : r.Y1 = r.Y2 : r.Y2 = t
    End Sub

    Private Sub InvalidateMap()
        Dim panel As Control = FindControl("pnlMap")
        If panel IsNot Nothing Then panel.Invalidate()
    End Sub

    ' ===== painting (called from MapPanel_Paint) =====
    Public Sub DrawRegions(g As Graphics)
        If regionList Is Nothing Then Return
        Dim selIndex As Integer = -1
        If lstRegions IsNot Nothing Then selIndex = lstRegions.SelectedIndex

        For i As Integer = 0 To regionList.Count - 1
            Dim r As MissionRegion = regionList(i)
            Dim col As Color = regionColors(i Mod regionColors.Length)
            Dim rect As System.Drawing.Rectangle = RegionScreenRect(r)

            Using fill As New SolidBrush(Color.FromArgb(60, col))
                g.FillRectangle(fill, rect)
            End Using
            Dim penWidth As Integer = If(i = selIndex, 3, 2)
            Using pen As New Pen(col, penWidth)
                g.DrawRectangle(pen, rect)
            End Using
            If r.Kind = RegionKind.Point Then
                Using cross As New Pen(col, 1)
                    g.DrawLine(cross, rect.Left, rect.Top, rect.Right, rect.Bottom)
                    g.DrawLine(cross, rect.Right, rect.Top, rect.Left, rect.Bottom)
                End Using
            End If

            ' name label
            Using bg As New SolidBrush(Color.FromArgb(180, Color.Black))
                Dim sz As SizeF = g.MeasureString(r.Name, Me.Font)
                g.FillRectangle(bg, rect.Left, rect.Top - sz.Height, sz.Width + 4, sz.Height)
                Using nameBrush As New SolidBrush(col)
                    g.DrawString(r.Name, Me.Font, nameBrush, rect.Left + 2, rect.Top - sz.Height)
                End Using
            End Using

            ' resize handles on the selected rectangle (edit mode)
            If i = selIndex AndAlso r.Kind = RegionKind.Rectangle Then
                Using hb As New SolidBrush(Color.White), hp As New Pen(Color.Black)
                    For Each hr As System.Drawing.Rectangle In HandleRects(rect).Values
                        g.FillRectangle(hb, hr)
                        g.DrawRectangle(hp, hr)
                    Next
                End Using
            End If
        Next

        ' beacons, walls/tubes and markers, then the units over them
        If showUnits Then DrawDozedBorders(g, Function(tx, ty) TilesToScreenRect(tx, ty, tx, ty))
        If showUnits Then DrawDroppedTubes(g, Function(tx, ty) TilesToScreenRect(tx, ty, tx, ty))
        If showMapObjects Then DrawMapObjects(g, Function(tx, ty) TilesToScreenRect(tx, ty, tx, ty), Me.Font)
        DrawUnits(g)

        ' in-progress drag rectangle
        If regionDrawing Then
            Dim t0x As Integer = Math.Min(regionStartTX, regionCurTX)
            Dim t0y As Integer = Math.Min(regionStartTY, regionCurTY)
            Dim t1x As Integer = Math.Max(regionStartTX, regionCurTX)
            Dim t1y As Integer = Math.Max(regionStartTY, regionCurTY)
            Dim rect As System.Drawing.Rectangle = TilesToScreenRect(t0x, t0y, t1x, t1y)
            Using pen As New Pen(Color.White, 2)
                pen.DashStyle = Drawing2D.DashStyle.Dash
                g.DrawRectangle(pen, rect)
            End Using
        End If
    End Sub

    Private Function RegionScreenRect(r As MissionRegion) As System.Drawing.Rectangle
        ' min/max so a region mid-resize (edges crossed) still draws as a proper rectangle
        Dim ax As Integer = LuaToTile(Math.Min(r.X1, r.X2))
        Dim ay As Integer = LuaToTile(Math.Min(r.Y1, r.Y2))
        Dim bx As Integer = LuaToTile(Math.Max(r.X1, r.X2))
        Dim by As Integer = LuaToTile(Math.Max(r.Y1, r.Y2))
        Return TilesToScreenRect(ax, ay, bx, by)
    End Function

    ' The 8 resize handles (corners + edge midpoints) for a screen rectangle, keyed by EditAction.
    Private Function HandleRects(rect As System.Drawing.Rectangle) As Dictionary(Of EditAction, System.Drawing.Rectangle)
        Dim d As New Dictionary(Of EditAction, System.Drawing.Rectangle)()
        Dim cx As Integer = rect.Left + rect.Width \ 2
        Dim cy As Integer = rect.Top + rect.Height \ 2
        d(EditAction.NW) = HandleBox(rect.Left, rect.Top)
        d(EditAction.N) = HandleBox(cx, rect.Top)
        d(EditAction.NE) = HandleBox(rect.Right, rect.Top)
        d(EditAction.E) = HandleBox(rect.Right, cy)
        d(EditAction.SE) = HandleBox(rect.Right, rect.Bottom)
        d(EditAction.S) = HandleBox(cx, rect.Bottom)
        d(EditAction.SW) = HandleBox(rect.Left, rect.Bottom)
        d(EditAction.W) = HandleBox(rect.Left, cy)
        Return d
    End Function

    Private Function HandleBox(x As Integer, y As Integer) As System.Drawing.Rectangle
        Return New System.Drawing.Rectangle(x - HandlePx \ 2, y - HandlePx \ 2, HandlePx, HandlePx)
    End Function

    ' Which resize handle (if any) is under the cursor for region r. Points have no handles.
    Private Function HitHandle(r As MissionRegion, px As Integer, py As Integer) As EditAction
        If r.Kind <> RegionKind.Rectangle Then Return EditAction.None
        Dim rect As System.Drawing.Rectangle = RegionScreenRect(r)
        For Each kv As KeyValuePair(Of EditAction, System.Drawing.Rectangle) In HandleRects(rect)
            If kv.Value.Contains(px, py) Then Return kv.Key
        Next
        Return EditAction.None
    End Function

    ' Is 0-based tile (tx,ty) inside region r (whose coords are 1-based lua)?
    Private Function TileInRegionLua(r As MissionRegion, tx As Integer, ty As Integer) As Boolean
        Dim ax As Integer = LuaToTile(Math.Min(r.X1, r.X2))
        Dim ay As Integer = LuaToTile(Math.Min(r.Y1, r.Y2))
        Dim bx As Integer = LuaToTile(Math.Max(r.X1, r.X2))
        Dim by As Integer = LuaToTile(Math.Max(r.Y1, r.Y2))
        Return tx >= ax AndAlso tx <= bx AndAlso ty >= ay AndAlso ty <= by
    End Function

    ' tile range (inclusive, 0-based) -> screen rectangle, honoring zoom + pan
    ' Both corners are rounded tile edges (TileEdgeX/Y), so neighbouring tiles share an edge pixel at any zoom: a
    ' width rounded on its own could leave a 1px gap or overlap (a darker line under two translucent fills).
    Private Function TilesToScreenRect(t0x As Integer, t0y As Integer, t1x As Integer, t1y As Integer) As System.Drawing.Rectangle
        Dim sx As Integer = TileEdgeX(t0x)
        Dim sy As Integer = TileEdgeY(t0y)
        Return New System.Drawing.Rectangle(sx, sy, TileEdgeX(t1x + 1) - sx, TileEdgeY(t1y + 1) - sy)
    End Function

    ' The screen pixel of the left (top) edge of 0-based tile column tx (row ty), where the scaled map draws it.
    Private Function TileEdgeX(tx As Integer) As Integer
        Return CInt(Math.Round(tx * 32 * zoomLevel)) + offsetX
    End Function
    Private Function TileEdgeY(ty As Integer) As Integer
        Return CInt(Math.Round(ty * 32 * zoomLevel)) + offsetY
    End Function

    ' ===== list management =====
    ' Clear all placed regions and units (called by Close Map) and refresh both list boxes.
    Public Sub ClearMissionData()
        regionList.Clear()
        unitsBlock = New PlacementText.UnitsBlock()
        mapObjects.Clear()
        RefreshRegionList()
        RefreshUnitList()
    End Sub

    ' A bold header line above a list.
    Private Function ListHeader(text As String) As Label
        Return New Label() With {.Text = text, .Dock = DockStyle.Top, .Height = 22, .Padding = New Padding(2, 0, 0, 0),
                                 .TextAlign = ContentAlignment.MiddleLeft, .Font = New Font(Me.Font, FontStyle.Bold)}
    End Function

    Private Function ListFont() As Font
        Return New Font("Segoe UI", 9.0F)
    End Function

    ' The lists take the side panel's height below the tile info; the split between them starts half way.
    Private Sub SizeListsSplit(props As Control)
        If listsSplit Is Nothing Then Return
        Dim h As Integer = Math.Max(220, props.ClientSize.Height - InfoHeight)
        If listsSplit.Height = h Then Return
        Dim share As Double = If(listsSplit.Height > 0, listsSplit.SplitterDistance / CDbl(listsSplit.Height), 0.4)
        listsSplit.Height = h
        listsSplit.SplitterDistance = Math.Max(60, Math.Min(h - 60, CInt(h * share)))
    End Sub

    ' What placement has on 0-based tile (tx, ty): units whose footprint covers it, beacons, walls and markers on
    ' it, and the regions it is in. One line each kind, for the side panel.
    Public Function TileContentsText(tx As Integer, ty As Integer) As String
        Dim lines As New List(Of String)()
        Dim us As New List(Of String)()
        For Each u As MissionUnit In unitList
            If Not u.HasAt Then Continue For
            Dim t0x, t0y, t1x, t1y As Integer
            UnitFootprintTiles(u, t0x, t0y, t1x, t1y)
            If tx >= t0x AndAlso tx <= t1x AndAlso ty >= t0y AndAlso ty <= t1y Then
                us.Add(u.UnitType & " p" & u.Player & If(String.IsNullOrEmpty(u.Name), "", " " & Chr(34) & u.Name & Chr(34)))
            End If
        Next
        If us.Count > 0 Then lines.Add("Unit: " & String.Join(", ", us))
        Dim os As New List(Of String)()
        For Each o As MissionMapObject In mapObjects
            If LuaToTile(o.X) = tx AndAlso LuaToTile(o.Y) = ty Then
                Dim what As String = o.ObjType
                If o.Kind = MapObjectKind.Beacon AndAlso o.ObjType = "MiningBeacon" Then what = o.Ore & " beacon"
                If o.Kind = MapObjectKind.Marker Then what &= " marker"
                os.Add(what & If(String.IsNullOrEmpty(o.Name), "", " " & Chr(34) & o.Name & Chr(34)))
            End If
        Next
        If os.Count > 0 Then lines.Add(String.Join(", ", os))
        Dim rs As New List(Of String)()
        For Each r As MissionRegion In regionList
            If TileInRegionLua(r, tx, ty) Then rs.Add(r.Name)
        Next
        If rs.Count > 0 Then lines.Add("In region: " & String.Join(", ", rs))
        Return String.Join(vbCrLf, lines)
    End Function

    ' Fill the regions list (and its header count) from regionList, keeping the selection.
    Private Sub RefreshRegionList()
        If lblRegionsHeader IsNot Nothing Then
            lblRegionsHeader.Text = "Regions (" & regionList.Count & ")   draw mode: drag = area, click = point"
        End If
        If lstRegions Is Nothing Then Return
        Dim keep As Integer = lstRegions.SelectedIndex
        lstRegions.BeginUpdate()
        lstRegions.Items.Clear()
        For Each r As MissionRegion In regionList
            lstRegions.Items.Add(r.ToString())
        Next
        lstRegions.EndUpdate()
        If keep >= 0 AndAlso keep < lstRegions.Items.Count Then lstRegions.SelectedIndex = keep
    End Sub

    Private Sub RenameRegion_Click(sender As Object, e As EventArgs)
        Dim i As Integer = If(lstRegions IsNot Nothing, lstRegions.SelectedIndex, -1)
        If i < 0 OrElse i >= regionList.Count Then Return
        Dim newName As String = InputBox("Region name:", "Rename Region", regionList(i).Name)
        If String.IsNullOrWhiteSpace(newName) Then Return
        regionList(i).Name = newName.Trim()
        RefreshRegionList()
        Dim panel As Control = FindControl("pnlMap") : If panel IsNot Nothing Then panel.Invalidate()
    End Sub

    Private Sub DeleteRegion_Click(sender As Object, e As EventArgs)
        Dim i As Integer = If(lstRegions IsNot Nothing, lstRegions.SelectedIndex, -1)
        If i < 0 OrElse i >= regionList.Count Then Return
        regionList.RemoveAt(i)
        RefreshRegionList()
        Dim panel As Control = FindControl("pnlMap") : If panel IsNot Nothing Then panel.Invalidate()
    End Sub

    Private Sub ClearRegions_Click(sender As Object, e As EventArgs)
        If regionList.Count = 0 Then Return
        If MessageBox.Show("Clear all regions?", "Confirm", MessageBoxButtons.YesNo, MessageBoxIcon.Question) = DialogResult.Yes Then
            regionList.Clear()
            RefreshRegionList()
            Dim panel As Control = FindControl("pnlMap") : If panel IsNot Nothing Then panel.Invalidate()
        End If
    End Sub

    ' ===== Mission Properties (the placement's header and the mission DLL built from it) =====
    ' The properties (name, map, tech, type, max_tech, players_count, unit_only, ai_players) live at the top of
    ' placement.lua, and OP2 reads the same values from the mission DLL before Lua runs. OK writes them into the
    ' placement and builds the DLL from them (MissionDll.SaveDll), so the two always agree.
    Private Sub MissionProps_Click(sender As Object, e As EventArgs)
        If String.IsNullOrEmpty(currentPlacementFile) OrElse Not File.Exists(currentPlacementFile) Then
            MessageBox.Show("Open a mission first (File > Open Mission) - properties are read from its placement.lua.",
                            "Mission Properties", MessageBoxButtons.OK, MessageBoxIcon.Information)
            Return
        End If

        Dim text As String
        Try
            text = File.ReadAllText(currentPlacementFile)
        Catch ex As Exception
            MessageBox.Show("Could not read placement file: " & ex.Message, "Mission Properties", MessageBoxButtons.OK, MessageBoxIcon.Error)
            Return
        End Try

        Dim hasAi As Boolean
        Dim old As MissionProps = MissionDll.ReadProps(text, MissionDll.ReadDllBeside(currentPlacementFile, hasAi))

        Using f As New fMissionProps()
            f.Props = old.Clone()
            f.Note = MissionDll.DllNote(currentPlacementFile, text)
            f.MapBrowseDir = MapBrowseRoot()
            If f.ShowDialog(Me) <> DialogResult.OK Then Return
            Dim p As MissionProps = f.Props

            Dim updated As String = MissionDll.WriteProps(text, p)
            Try
                File.Copy(currentPlacementFile, currentPlacementFile & ".bak", True)
                File.WriteAllText(currentPlacementFile, updated)
            Catch ex As Exception
                MessageBox.Show("Could not save placement file: " & ex.Message, "Mission Properties", MessageBoxButtons.OK, MessageBoxIcon.Error)
                Return
            End Try

            Dim dllNote As String
            Try
                dllNote = vbCrLf & vbCrLf & MissionDll.SaveDll(currentPlacementFile, updated, FindBundledStub())
            Catch ex As Exception
                dllNote = vbCrLf & vbCrLf & "The mission DLL was not written: " & ex.Message
            End Try

            ' If the map changed, reload the new map onto the canvas (overlay regions/units unchanged).
            If Not String.Equals(p.MapFile, old.MapFile, StringComparison.OrdinalIgnoreCase) Then
                Dim newMapPath As String = ResolveMapPath(p.MapFile, Path.GetDirectoryName(currentPlacementFile))
                If String.IsNullOrEmpty(newMapPath) Then
                    dllNote &= vbCrLf & vbCrLf & "(Could not find """ & p.MapFile & """ to display - it will still apply in-game.)"
                ElseIf LoadMapFile(newMapPath) Then
                    RefreshRegionList()
                    RefreshUnitList()
                    Dim panel As Control = FindControl("pnlMap") : If panel IsNot Nothing Then panel.Invalidate()
                End If
            End If

            Text = ApplicationName & " - " & p.Name

            MessageBox.Show("Mission properties saved to " & Path.GetFileName(currentPlacementFile) & "." & dllNote,
                            "Mission Properties", MessageBoxButtons.OK, MessageBoxIcon.Information)
        End Using
    End Sub

    ' --- placement.lua scalar metadata helpers (first occurrence = the top-level metadata) ---

    Private Function ScalarString(text As String, key As String) As String
        Dim m As Match = Regex.Match(text, "\b" & Regex.Escape(key) & "\s*=\s*""([^""]*)""")
        Return If(m.Success, m.Groups(1).Value, "")
    End Function

    ' Folder the "Change Map..." dialog should open in: <OP2>\OPU (falls back to the OP2 path).
    Private Function MapBrowseRoot() As String
        Dim base As String = My.Settings.OP2Path
        If String.IsNullOrEmpty(base) Then Return ""
        Dim opu As String = Path.Combine(base, "OPU")
        Return If(Directory.Exists(opu), opu, base)
    End Function

    ' ===== Open Mission (placement-first entry point) =====
    ' The mission is the entry point, not the map. Pick either  HoldTheLine.lua  or  HoldTheLine.placement.lua;
    ' we resolve BOTH siblings, read the map name from the placement file's  map = "..."  header, scan the
    ' OP2/OPU folders for that .map, load it, then lay the regions/units overlay over it - one action.
    Private currentPlacementFile As String = Nothing
    Private currentMissionScript As String = Nothing

    Private lastLoadedMapName As String = ""    ' map name reported by the last successful LoadMission

    Private Sub OpenMission_Click(sender As Object, e As EventArgs)
        Using dlg As New OpenFileDialog()
            dlg.Title = "Open Mission - select the mission .lua or its .placement.lua"
            dlg.Filter = "OP2Lua mission (*.lua)|*.lua|All files (*.*)|*.*"
            If Not String.IsNullOrEmpty(My.Settings.WorkingPath) AndAlso Directory.Exists(My.Settings.WorkingPath) Then
                dlg.InitialDirectory = My.Settings.WorkingPath
            ElseIf Not String.IsNullOrEmpty(My.Settings.OP2Path) Then
                dlg.InitialDirectory = My.Settings.OP2Path
            End If
            If dlg.ShowDialog() <> DialogResult.OK Then Return

            OpenMissionFile(dlg.FileName)
        End Using
    End Sub

    ' Open a mission from either of its files (<Name>.lua or <Name>.placement.lua): File > Open Mission, a file
    ' dropped on the map, or the command line.
    Public Function OpenMissionFile(picked As String) As Boolean
        ' Work out the placement file regardless of which sibling was picked.
        Dim placementFile As String
        If picked.ToLowerInvariant().EndsWith(".placement.lua") Then
            placementFile = picked
        Else
            placementFile = picked.Substring(0, picked.Length - ".lua".Length) & ".placement.lua"
        End If

        If Not File.Exists(placementFile) Then
            MessageBox.Show("Could not find the placement file:" & vbCrLf & placementFile & vbCrLf & vbCrLf &
                            "An OP2Lua mission needs a matching <name>.placement.lua beside <name>.lua.",
                            "Open Mission", MessageBoxButtons.OK, MessageBoxIcon.Error)
            Return False
        End If

        If Not LoadMission(placementFile) Then Return False

        Dim scriptNote As String = If(currentMissionScript Is Nothing, "(no matching mission .lua found)", Path.GetFileName(currentMissionScript))
        MessageBox.Show("Mission loaded." & vbCrLf & vbCrLf &
                        "Script:    " & scriptNote & vbCrLf &
                        "Placement: " & Path.GetFileName(placementFile) & vbCrLf &
                        "Map:       " & lastLoadedMapName & vbCrLf & vbCrLf &
                        regionList.Count & " region(s), " & unitList.Count & " unit(s)" & MapObjectSummary() & ".",
                        "Open Mission", MessageBoxButtons.OK, MessageBoxIcon.Information)
        Return True
    End Function

    ' Core mission loader: given a placement.lua path, derive the script sibling, read+resolve+load its
    ' map, parse the regions/units overlay, and wire up mission state. Shows error dialogs and returns
    ' False on failure; shows NO success UI (the caller does). Shared by Open Mission and New Mission.
    Private Function LoadMission(placementFile As String) As Boolean
        If Not File.Exists(placementFile) Then
            MessageBox.Show("Placement file not found:" & vbCrLf & placementFile, "Open Mission", MessageBoxButtons.OK, MessageBoxIcon.Error)
            Return False
        End If
        Dim dir As String = Path.GetDirectoryName(placementFile)
        Dim scriptFile As String = Nothing
        If placementFile.ToLowerInvariant().EndsWith(".placement.lua") Then
            scriptFile = placementFile.Substring(0, placementFile.Length - ".placement.lua".Length) & ".lua"
        End If

        Dim placementText As String
        Try
            placementText = File.ReadAllText(placementFile)
        Catch ex As Exception
            MessageBox.Show("Could not read placement file: " & ex.Message, "Open Mission", MessageBoxButtons.OK, MessageBoxIcon.Error)
            Return False
        End Try

        Dim mapName As String = Nothing
        Dim mm As Match = Regex.Match(placementText, "\bmap\s*=\s*""([^""]+)""", RegexOptions.IgnoreCase)
        If mm.Success Then mapName = mm.Groups(1).Value.Trim()
        If String.IsNullOrEmpty(mapName) Then
            MessageBox.Show("This placement file does not declare a map (expected something like  map = ""eden01.map"" )." & vbCrLf &
                            placementFile, "Open Mission", MessageBoxButtons.OK, MessageBoxIcon.Error)
            Return False
        End If

        Dim mapPath As String = ResolveMapPath(mapName, dir)
        If String.IsNullOrEmpty(mapPath) Then
            MessageBox.Show("Could not find the map """ & mapName & """ for this mission." & vbCrLf & vbCrLf &
                            "Looked in:" & vbCrLf & DescribeMapSearchDirs(dir),
                            "Map Not Found", MessageBoxButtons.OK, MessageBoxIcon.Error)
            Return False
        End If

        If Not LoadMapFile(mapPath) Then Return False

        currentPlacementFile = placementFile
        currentMissionScript = If(scriptFile IsNot Nothing AndAlso File.Exists(scriptFile), scriptFile, Nothing)
        UpdateMissionLabel()
        lastLoadedMapName = mapName
        If mnuMissionProps IsNot Nothing Then mnuMissionProps.Enabled = True
        If mnuTestInOp2 IsNot Nothing Then mnuTestInOp2.Enabled = True

        LoadOverlay(placementText)
        RefreshRegionList()
        RefreshUnitList()

        ' Title the window after the mission (placement details.name), not the .map LoadMapFile set.
        Dim missionName As String = ScalarString(placementText, "name")
        If String.IsNullOrEmpty(missionName) Then missionName = Path.GetFileNameWithoutExtension(placementFile)
        Text = ApplicationName & " - " & missionName

        Dim panel As Control = FindControl("pnlMap") : If panel IsNot Nothing Then panel.Invalidate()
        Return True
    End Function

    ' ===== Test in OP2 (boot OP2 straight into the open mission's DLL via op2launcher.exe) =====
    Private Sub TestInOp2_Click(sender As Object, e As EventArgs)
        If String.IsNullOrEmpty(currentPlacementFile) Then
            MessageBox.Show("Open a mission first (Mission -> Open Mission, or New Mission).",
                            "Test in OP2", MessageBoxButtons.OK, MessageBoxIcon.Information)
            Return
        End If

        ' Derive the sibling mission DLL from the placement file (same base-name convention as the rest
        ' of the editor). op2launcher boots a DLL by name, so the 'c' colony-menu prefix is optional -
        ' prefer cBase.dll if present, otherwise Base.dll.
        Dim baseName As String = Path.GetFileName(currentPlacementFile)
        If baseName.ToLowerInvariant().EndsWith(".placement.lua") Then baseName = baseName.Substring(0, baseName.Length - ".placement.lua".Length)
        Dim dir As String = Path.GetDirectoryName(currentPlacementFile)
        Dim dllName As String = "c" & baseName & ".dll"
        If Not File.Exists(Path.Combine(dir, dllName)) Then dllName = baseName & ".dll"
        If Not File.Exists(Path.Combine(dir, dllName)) Then
            MessageBox.Show("No mission DLL found beside the placement file (looked for c" & baseName & ".dll and " & baseName & ".dll)." & vbCrLf & vbCrLf &
                            "Create the mission via New Mission so its DLL exists, or place it in OPU\maps\LUA.",
                            "Test in OP2", MessageBoxButtons.OK, MessageBoxIcon.Warning)
            Return
        End If

        ' The game runs the files on disk: pick up changes made to it elsewhere, then offer to save edits first.
        If Not ReconcileWithDisk() Then Return
        If MissionHasUnsavedEdits() Then
            Select Case MessageBox.Show("Save your changes to " & Path.GetFileName(currentPlacementFile) & " before testing?",
                                        "Test in OP2", MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question)
                Case DialogResult.Yes : SaveMissionToFile(currentPlacementFile)
                Case DialogResult.Cancel : Return
            End Select
        End If

        ' op2launcher boots a mission by DLL name, which the game looks up in its own folders, and OP2LuaCore loads
        ' the scripts from beside the DLL. A mission kept elsewhere is copied into OPU\maps\LUA first.
        If Not DeployMission(dir, baseName, dllName) Then Return
        LaunchMissionDll(dllName)
    End Sub

    ' Copy a mission's DLL, script and placement into <OP2>\OPU\maps\LUA (nothing to do when it is already there).
    ' A file there that differs from the mission's is replaced only after asking: it may be another mission of the
    ' same name.
    Private Function DeployMission(dir As String, baseName As String, dllName As String) As Boolean
        If String.IsNullOrEmpty(My.Settings.OP2Path) Then Return True     ' LaunchMissionDll says what is missing
        Dim deployDir As String = Path.Combine(My.Settings.OP2Path, "OPU", "maps", "LUA")
        If SamePath(dir, deployDir) Then Return True

        Dim names As New List(Of String) From {dllName, baseName & ".lua", baseName & ".placement.lua"}
        Dim copies As New List(Of String)()
        Dim replaced As New List(Of String)()
        For Each n As String In names
            Dim src As String = Path.Combine(dir, n)
            If Not File.Exists(src) Then Continue For
            Dim dst As String = Path.Combine(deployDir, n)
            copies.Add(n)
            ' A file this editor copied there earlier (this session) is simply updated.
            If File.Exists(dst) AndAlso Not deployedFiles.Contains(Path.GetFullPath(dst)) AndAlso Not FilesEqual(src, dst) Then
                replaced.Add(n)
            End If
        Next
        If replaced.Count > 0 Then
            Dim ask As String = "Test in OP2 copies the mission into:" & vbCrLf & deployDir & vbCrLf & vbCrLf &
                                "These files there are different and will be replaced:" & vbCrLf & "  " &
                                String.Join(vbCrLf & "  ", replaced) & vbCrLf & vbCrLf & "Replace them?"
            If MessageBox.Show(ask, "Test in OP2", MessageBoxButtons.YesNo, MessageBoxIcon.Question) <> DialogResult.Yes Then Return False
        End If
        Try
            Directory.CreateDirectory(deployDir)
            For Each n As String In copies
                File.Copy(Path.Combine(dir, n), Path.Combine(deployDir, n), True)
                deployedFiles.Add(Path.GetFullPath(Path.Combine(deployDir, n)))
            Next
        Catch ex As Exception
            MessageBox.Show("Could not copy the mission into " & deployDir & ":" & vbCrLf & ex.Message,
                            "Test in OP2", MessageBoxButtons.OK, MessageBoxIcon.Error)
            Return False
        End Try
        Return True
    End Function

    ' The open placement file's text as the editor last read or wrote it.
    Private loadedPlacementText As String = Nothing

    ' Before saving over (or testing) the open mission: if its file changed on disk since the editor read it (another
    ' editor, a converter), reload it when there are no edits here, else ask. False = stop.
    Private Function ReconcileWithDisk() As Boolean
        If String.IsNullOrEmpty(currentPlacementFile) OrElse Not File.Exists(currentPlacementFile) OrElse loadedPlacementText Is Nothing Then Return True
        Dim disk As String = File.ReadAllText(currentPlacementFile)
        If disk = loadedPlacementText Then Return True
        If Not MissionHasUnsavedEdits() Then
            LoadOverlay(disk)
            InvalidateMap()
            Return True
        End If
        Select Case MessageBox.Show(Path.GetFileName(currentPlacementFile) & " has changed on disk since it was opened, and it has edits here too." &
                                    vbCrLf & vbCrLf & "Yes: keep the file on disk (reload it; the edits here are dropped)." &
                                    vbCrLf & "No: keep the edits here (they replace the file's units and regions)." &
                                    vbCrLf & "Cancel: do nothing.",
                                    "File Changed", MessageBoxButtons.YesNoCancel, MessageBoxIcon.Warning)
            Case DialogResult.Yes
                LoadOverlay(disk)
                InvalidateMap()
                Return True
            Case DialogResult.No
                Return True
            Case Else
                Return False
        End Select
    End Function

    ' Whether the regions or units differ from the open placement file's: the units as the text Save would write,
    ' the regions by name and corners (Save rewrites their table's layout, so its text isn't compared).
    Private Function MissionHasUnsavedEdits() As Boolean
        If String.IsNullOrEmpty(currentPlacementFile) OrElse loadedPlacementText Is Nothing Then Return False
        Dim text As String = loadedPlacementText
        ' (A file with no units table and none placed: Save would add an empty one, which isn't an edit.)
        If (unitsBlock.Found OrElse unitList.Count > 0) AndAlso WriteUnits(text, currentPlacementFile) <> text Then Return True
        Dim onDisk As List(Of MissionRegion) = PlacementText.ParseRegions(text)
        If onDisk.Count <> regionList.Count Then Return True
        For i As Integer = 0 To onDisk.Count - 1
            If onDisk(i).ToString() <> regionList(i).ToString() Then Return True
        Next
        Return False
    End Function

    ' The open mission in the status bar: its script <Name>.lua, or the placement file when it has no script beside it;
    ' the full path on hover. "No mission" when none is open.
    Public Sub UpdateMissionLabel()
        Dim f As String = If(currentMissionScript, currentPlacementFile)
        lblToolStripMission.Text = If(String.IsNullOrEmpty(f), "No mission", Path.GetFileName(f))
        lblToolStripMission.ToolTipText = If(f, "")
    End Sub

    ' ===== the game's OP2LuaCore.dll (status bar) =====
    Private lblToolStripCore As ToolStripStatusLabel = Nothing

    ' Show which OP2LuaCore.dll the game folder has: its version and when it was built (the link time in its PE
    ' header, which a copy keeps; the version alone doesn't change between builds). Hover for the path. Called at
    ' start-up and after Settings.
    Public Sub UpdateCoreLabel()
        If lblToolStripCore Is Nothing Then
            lblToolStripCore = New ToolStripStatusLabel() With {.Name = "lblToolStripCore"}
            statusStrip.Items.Add(lblToolStripCore)
        End If
        Dim core As String = If(String.IsNullOrEmpty(My.Settings.OP2Path), "", Path.Combine(My.Settings.OP2Path, "OPU", "OP2LuaCore.dll"))
        If core = "" OrElse Not File.Exists(core) Then
            lblToolStripCore.Text = "OP2LuaCore: not installed"
            lblToolStripCore.ForeColor = Color.Firebrick
            lblToolStripCore.ToolTipText = If(core = "", "Set the Outpost 2 folder in Settings.", "No " & core & " - OP2Lua missions can't run.")
            Return
        End If
        Dim ver As String = FileVersionInfo.GetVersionInfo(core).FileVersion
        Dim built As String = ""
        Try
            Dim buf() As Byte = File.ReadAllBytes(core)
            Dim stamp As Integer = BitConverter.ToInt32(buf, BitConverter.ToInt32(buf, &H3C) + 8)   ' COFF TimeDateStamp
            built = ", built " & DateTimeOffset.FromUnixTimeSeconds(CUInt(stamp)).LocalDateTime.ToString("yyyy-MM-dd HH:mm")
        Catch ex As Exception
        End Try
        lblToolStripCore.Text = "OP2LuaCore " & If(String.IsNullOrEmpty(ver), "?", ver) & built
        lblToolStripCore.ForeColor = SystemColors.ControlText
        lblToolStripCore.ToolTipText = core
    End Sub

    ' Files Test in OP2 has copied into the game this session (full paths, any case).
    Private ReadOnly deployedFiles As New HashSet(Of String)(StringComparer.OrdinalIgnoreCase)

    ' Byte-for-byte the same file (length first, so a differing size costs nothing).
    Private Function FilesEqual(a As String, b As String) As Boolean
        Dim fa As New FileInfo(a), fb As New FileInfo(b)
        If fa.Length <> fb.Length Then Return False
        Return File.ReadAllBytes(a).SequenceEqual(File.ReadAllBytes(b))
    End Function

    ''' <summary>
    ''' Launches Outpost 2 straight into a mission DLL via op2launcher.exe in
    ''' the OPU folder. op2launcher arms the in-game Launcher add-on (one-shot)
    ''' and boots the game directly into the given DLL - no menus. Used for
    ''' "Test in OP2": run a map through an editor mission DLL.
    '''
    ''' missionDll is the DLL name as it lives in the OPU folder (e.g.
    ''' "cEditorMission.dll"), not a full path. Returns True if the launch was
    ''' started; failure cases show the user a MessageBox.
    ''' </summary>
    Public Function LaunchMissionDll(missionDll As String) As Boolean
        If String.IsNullOrWhiteSpace(missionDll) Then
            MessageBox.Show("No mission DLL was specified.",
                            "Launch Mission",
                            MessageBoxButtons.OK, MessageBoxIcon.Warning)
            Return False
        End If

        ' Verify the configured OP2 directory exists.
        If String.IsNullOrEmpty(My.Settings.OP2Path) OrElse
           Not Directory.Exists(My.Settings.OP2Path) Then
            MessageBox.Show(
                "Outpost 2 game directory is not set or doesn't exist." & vbCrLf & vbCrLf &
                "Configure it in File -> Settings.",
                "Launch Mission",
                MessageBoxButtons.OK, MessageBoxIcon.Warning)
            Return False
        End If

        Dim opuFolder As String = Path.Combine(My.Settings.OP2Path, "OPU")
        Dim launcherPath As String = Path.Combine(opuFolder, "op2launcher.exe")

        If Not File.Exists(launcherPath) Then
            MessageBox.Show(
                "op2launcher.exe was not found at:" & vbCrLf & launcherPath & vbCrLf & vbCrLf &
                "Copy op2launcher.exe and Launcher.dll into the OPU folder (next to OPULauncher.exe).",
                "Launch Mission",
                MessageBoxButtons.OK, MessageBoxIcon.Error)
            Return False
        End If

        Try
            ' op2launcher must run from the OPU folder - it resolves the mission
            ' DLL and outpost2.ini relative to its own location.
            Dim startInfo As New ProcessStartInfo() With {
                .FileName = launcherPath,
                .Arguments = """" & missionDll & """",
                .WorkingDirectory = opuFolder
            }
            Process.Start(startInfo)
            Return True
        Catch ex As Exception
            MessageBox.Show($"Failed to launch mission: {ex.Message}",
                            "Launch Mission",
                            MessageBoxButtons.OK, MessageBoxIcon.Error)
            Return False
        End Try
    End Function

    ' ===== New Mission (scaffold a fresh mission: clone+patch the bundled stub, write starter lua) =====
    ' Builds the mission DLL from the dialog's properties (a copy of the bundled LuaMission.dll with each value at its
    ' export, MissionDll.BuildDll), writes a starter <Base>.lua + <Base>.placement.lua into <OP2>\OPU\maps\LUA,
    ' then opens it.
    Private Sub NewMission_Click(sender As Object, e As EventArgs)
        If String.IsNullOrEmpty(My.Settings.OP2Path) Then
            MessageBox.Show("Set the Outpost 2 game directory in Settings first - new missions are written under it.",
                            "New Mission", MessageBoxButtons.OK, MessageBoxIcon.Information)
            Return
        End If

        Dim stubPath As String = FindBundledStub()
        If String.IsNullOrEmpty(stubPath) Then
            MessageBox.Show("Bundled LuaMission.dll stub not found beside the editor (expected in a 'stub' folder next to the exe).",
                            "New Mission", MessageBoxButtons.OK, MessageBoxIcon.Error)
            Return
        End If

        Using f As New fMissionProps()
            f.Text = "New Mission"
            f.Props = New MissionProps()
            f.MapBrowseDir = MapBrowseRoot()
            If f.ShowDialog(Me) <> DialogResult.OK Then Return
            Dim p As MissionProps = f.Props

            Dim base As String = BaseNameFrom(p.Name)
            If String.IsNullOrEmpty(base) Then
                MessageBox.Show("The mission name needs at least one letter or digit.", "New Mission", MessageBoxButtons.OK, MessageBoxIcon.Warning)
                Return
            End If

            ' OP2 1.4.1 loads missions flat from OPU\maps\LUA - write the three files straight in there. Outpost 2
            ' lists a DLL whose name starts with c under Colony Games; the core finds cMyMission.dll's scripts as
            ' MyMission.*, dropping a c before a capital, so a base without a leading capital keeps the c throughout.
            Dim colony As Boolean = Not PlacementText.IsMultiplayerType(p.TypeName)
            Dim dllBase As String = If(colony, "c" & base, base)
            If colony AndAlso Not Char.IsUpper(base(0)) Then base = dllBase
            Dim outDir As String = Path.Combine(My.Settings.OP2Path, "OPU", "maps", "LUA")
            Dim dllOut As String = Path.Combine(outDir, dllBase & ".dll")
            Dim placementOut As String = Path.Combine(outDir, base & ".placement.lua")
            Dim scriptOut As String = Path.Combine(outDir, base & ".lua")

            If File.Exists(dllOut) OrElse File.Exists(placementOut) OrElse File.Exists(scriptOut) Then
                If MessageBox.Show("A mission already exists in:" & vbCrLf & outDir & vbCrLf & vbCrLf & "Overwrite its files?",
                                   "New Mission", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) <> DialogResult.Yes Then Return
            End If

            Try
                Directory.CreateDirectory(outDir)

                File.WriteAllBytes(dllOut, MissionDll.BuildDll(File.ReadAllBytes(stubPath), p))

                ' Starter lua files.
                File.WriteAllText(placementOut, NewPlacementSkeleton(base, p))
                File.WriteAllText(scriptOut, NewMissionSkeleton(base, p.Name, p.TypeName))
            Catch ex As Exception
                MessageBox.Show("Could not create mission: " & ex.Message, "New Mission", MessageBoxButtons.OK, MessageBoxIcon.Error)
                Return
            End Try

            ' Open the new mission straight away.
            If LoadMission(placementOut) Then
                Text = ApplicationName & " - " & p.Name
                MessageBox.Show("Created mission """ & p.Name & """ in:" & vbCrLf & outDir & vbCrLf & vbCrLf &
                                dllBase & ".dll   (the mission's properties, as OP2 reads them)" & vbCrLf &
                                base & ".lua   (your logic)" & vbCrLf &
                                base & ".placement.lua   (your layout)" & vbCrLf & vbCrLf &
                                "Those three files are the mission - copy them where Outpost 2 loads missions to play.",
                                "New Mission", MessageBoxButtons.OK, MessageBoxIcon.Information)
            End If
        End Using
    End Sub

    ' Mission base name = the name with everything except letters/digits stripped (matches op2lua-newmission).
    Private Function BaseNameFrom(name As String) As String
        Dim sb As New StringBuilder()
        For Each c As Char In name
            If Char.IsLetterOrDigit(c) Then sb.Append(c)
        Next
        Return sb.ToString()
    End Function

    ' Locate the stub DLL: beside the editor (its build output copies it into stub\), or in the SDK's bin\ next to
    ' the tools\ folder the editor ships in.
    Private Function FindBundledStub() As String
        Dim exeDir As String = Application.StartupPath
        For Each c As String In New String() {
            Path.Combine(exeDir, "stub", "LuaMission.dll"),
            Path.Combine(exeDir, "LuaMission.dll"),
            Path.GetFullPath(Path.Combine(exeDir, "..", "bin", "LuaMission.dll"))}
            If File.Exists(c) Then Return c
        Next
        Return Nothing
    End Function

    ' The starter placement.lua for New Mission: a small two-player fight, or for a multiplayer type a Command Center per
    ' player and no players table (the lobby sets each player's colony and colour).
    Private Function NewPlacementSkeleton(base As String, p As MissionProps) As String
        Dim name As String = p.Name
        Dim missionType As String = p.TypeName
        Dim players As Integer = p.Players
        Dim q As Func(Of String, String) = Function(s) """" & s & """"
        Dim sb As New StringBuilder()
        sb.AppendLine("-- " & base & ".placement.lua  -  " & name & "  -  starting layout.")
        sb.AppendLine("-- Coordinates are the ones shown on the in-game status bar (hover a tile to read them).")
        sb.AppendLine("return {")
        sb.AppendLine("  name = " & q(name) & ", map = " & q(p.MapFile) & ", tech = " & q(p.Tech) & ", type = " & q(missionType) & ",")
        sb.AppendLine("  max_tech = " & p.MaxTech & ", players_count = " & players & ",")
        If p.UnitOnly OrElse p.AiPlayers <> 0 Then
            sb.AppendLine("  unit_only = " & If(p.UnitOnly, "true", "false") & ", ai_players = " & p.AiPlayers & ",")
        End If
        sb.AppendLine("")
        If PlacementText.IsMultiplayerType(missionType) Then
            ' A multiplayer game: the lobby sets each player's colony, colour and human/AI; a players table would
            ' override them. players[1..n] exist in the mission script either way.
            sb.AppendLine("  -- No players table: in a multiplayer game the lobby has already set each player's colony and colour.")
            sb.AppendLine("")
            sb.AppendLine("  units = {")
            sb.AppendLine("    -- What every game starts with (player = 1.." & players & "). Bases that depend on the lobby's")
            sb.AppendLine("    -- settings (game.initial_units()) are made in on_init instead.")
            For k As Integer = 1 To players
                sb.AppendLine("    { type = ""CommandCenter"", player = " & k & ", at = { " & (20 + 12 * (k - 1)) & ", 20 } },")
            Next
            sb.AppendLine("  },")
            sb.AppendLine("")
            sb.AppendLine("  beacons = {}, walls = {},")
            sb.AppendLine("  regions = {},")
            sb.AppendLine("  markers = {},")
            sb.AppendLine("}")
            Return sb.ToString()
        End If
        sb.AppendLine("  players = {")
        sb.AppendLine("    [1] = { colony = ""Eden"", human = true, color = ""Blue"",")
        sb.AppendLine("            resources = { common_ore = 3000, food = 3000, kids = 8, workers = 12, scientists = 6, tech_level = 9 },")
        sb.AppendLine("            center_view = { 30, 55 } },")
        sb.AppendLine("    [2] = { colony = ""Plymouth"", human = false, color = ""Red"",")
        sb.AppendLine("            resources = { common_ore = 4000, food = 4000, kids = 15, workers = 20, scientists = 10, tech_level = 12 } },")
        sb.AppendLine("  },")
        sb.AppendLine("")
        sb.AppendLine("  units = {")
        sb.AppendLine("    -- Your starting force (player 1).")
        sb.AppendLine("    { type = ""Lynx"",          player = 1, at = { 30, 54 }, weapon = ""Laser"" },")
        sb.AppendLine("    { type = ""Lynx"",          player = 1, at = { 31, 54 }, weapon = ""RPG"" },")
        sb.AppendLine("    { type = ""CommandCenter"", player = 1, at = { 28, 56 } },")
        sb.AppendLine("    { type = ""Tokamak"",       player = 1, at = { 31, 57 } },")
        sb.AppendLine("")
        sb.AppendLine("    -- The enemy you fight (player 2). A powered Guard Post will fire at you.")
        sb.AppendLine("    { type = ""CommandCenter"", player = 2, at = { 58, 44 } },")
        sb.AppendLine("    { type = ""Tokamak"",       player = 2, at = { 60, 44 } },")
        sb.AppendLine("    { type = ""GuardPost"",     player = 2, at = { 58, 46 }, weapon = ""RPG"" },")
        sb.AppendLine("  },")
        sb.AppendLine("")
        sb.AppendLine("  beacons = {}, walls = {},")
        sb.AppendLine("  regions = { enemy_base = { 54, 40, 64, 48 } },   -- a rectangle: { x1, y1, x2, y2 }")
        sb.AppendLine("  markers = {},")
        sb.AppendLine("}")
        Return sb.ToString()
    End Function

    ' The starter <Base>.lua for New Mission: win / lose on the Command Centers, or for a multiplayer type the lobby's
    ' settings and the type's victory.
    Private Function NewMissionSkeleton(base As String, name As String, missionType As String) As String
        Dim sb As New StringBuilder()
        sb.AppendLine("-- " & base & ".lua  -  " & name & "  -  mission logic.")
        sb.AppendLine("-- See docs/API.md for the full API. Edit freely; this is your mission.")
        sb.AppendLine("")
        If PlacementText.IsMultiplayerType(missionType) Then
            sb.AppendLine("function on_init()")
            sb.AppendLine("  -- The lobby's settings: game.initial_units(), game.uses_day_night(), game.uses_morale(),")
            sb.AppendLine("  -- game.disasters_enabled(), and each player's .difficulty (the resource level).")
            If PlacementText.MissionTypeValue(missionType) = -8 Then
                sb.AppendLine("  mission.last_one_standing()   -- the game's own Last One Standing victory, per player")
            Else
                sb.AppendLine("  -- Win: this type's victory with trigger.* and mission.victory (docs/API.md, ""Multiplayer missions"").")
            End If
            sb.AppendLine("end")
            Return sb.ToString()
        End If
        sb.AppendLine("function on_init()")
        sb.AppendLine("  game.message(""" & name & ": destroy the enemy Command Center!"")")
        sb.AppendLine("  game.sound(""NewMissionObjective"")")
        sb.AppendLine("  game.morale_steady()   -- keep morale stable so it's about the fight, not colony micro")
        sb.AppendLine("")
        sb.AppendLine("  -- WIN: the enemy has no Command Center left.")
        sb.AppendLine("  when(function() return not players[2]:owns_any(""CommandCenter"") end, function()")
        sb.AppendLine("    game.message(""Enemy base destroyed - mission accomplished!"")")
        sb.AppendLine("    game.sound(""StructureDestroyed"")")
        sb.AppendLine("    mission.win()")
        sb.AppendLine("  end)")
        sb.AppendLine("")
        sb.AppendLine("  -- LOSE: your colony's Command Center is gone.")
        sb.AppendLine("  when(function() return not players[1]:owns_any(""CommandCenter"") end, function()")
        sb.AppendLine("    game.message(""Your Command Center is destroyed. Mission failed."")")
        sb.AppendLine("    mission.lose()")
        sb.AppendLine("  end)")
        sb.AppendLine("end")
        Return sb.ToString()
    End Function

    ' Resolve a bare map filename (e.g. "newworld.map") to a full path. Works for both the flat 1.3.6
    ' layout (game root holds Outpost2.exe + mission DLLs/lua + the .map files together) and the 1.4.1
    ' OPU layout (maps organised under OPU subfolders). Scans, stopping at the first hit:
    '   1) the mission folder itself          - the 1.3.6 case (map sits beside the placement.lua)
    '   2) <OP2>                              (game root, flat layout)
    '   3) <OP2>\maps                + subfolders
    '   4) <OP2>\OPU                          (top level)
    '   5) <OP2>\OPU\maps            + subfolders
    '   6) <OP2>\OPU\base\maps       + subfolders
    ' where <OP2> is the game directory from Settings. Returns Nothing if not found.
    Private Function ResolveMapPath(mapName As String, missionDir As String) As String
        If Not String.IsNullOrEmpty(missionDir) Then
            Dim local As String = Path.Combine(missionDir, mapName)
            If File.Exists(local) Then Return local
        End If

        Dim base As String = My.Settings.OP2Path
        If String.IsNullOrEmpty(base) Then Return Nothing

        Dim hit As String = FindMapInDir(base, mapName, False)                                 : If hit IsNot Nothing Then Return hit
        hit = FindMapInDir(Path.Combine(base, "maps"), mapName, True)                          : If hit IsNot Nothing Then Return hit
        hit = FindMapInDir(Path.Combine(base, "OPU"), mapName, False)                          : If hit IsNot Nothing Then Return hit
        hit = FindMapInDir(Path.Combine(base, "OPU", "maps"), mapName, True)                   : If hit IsNot Nothing Then Return hit
        hit = FindMapInDir(Path.Combine(base, "OPU", "base", "maps"), mapName, True)           : If hit IsNot Nothing Then Return hit

        Return Nothing
    End Function

    ' Find a file named mapName directly in root (recursive:=False) or anywhere beneath it
    ' (recursive:=True). Case-insensitive on Windows. Returns the full path or Nothing.
    Private Function FindMapInDir(root As String, mapName As String, recursive As Boolean) As String
        Try
            If String.IsNullOrEmpty(root) OrElse Not Directory.Exists(root) Then Return Nothing
            If Not recursive Then
                Dim direct As String = Path.Combine(root, mapName)
                Return If(File.Exists(direct), direct, Nothing)
            End If
            Dim matches As String() = Directory.GetFiles(root, mapName, SearchOption.AllDirectories)
            Return If(matches.Length > 0, matches(0), Nothing)
        Catch
            Return Nothing
        End Try
    End Function

    ' Human-readable list of the folders ResolveMapPath scans (for the "not found" message).
    Private Function DescribeMapSearchDirs(missionDir As String) As String
        Dim base As String = If(String.IsNullOrEmpty(My.Settings.OP2Path), "(OP2 path not set in Settings)", My.Settings.OP2Path)
        Dim sb As New StringBuilder()
        If Not String.IsNullOrEmpty(missionDir) Then sb.AppendLine("  " & missionDir & "  (mission folder)")
        sb.AppendLine("  " & base & "  (game root)")
        sb.AppendLine("  " & Path.Combine(base, "maps") & "  (+ subfolders)")
        sb.AppendLine("  " & Path.Combine(base, "OPU"))
        sb.AppendLine("  " & Path.Combine(base, "OPU", "maps") & "  (+ subfolders)")
        sb.AppendLine("  " & Path.Combine(base, "OPU", "base", "maps") & "  (+ subfolders)")
        Return sb.ToString()
    End Function

    ' ===== unified save: write BOTH regions and units back to the open mission's placement.lua =====
    ' This is the one-click save (also Ctrl+S). It targets the file opened via Open Mission / Load
    ' Regions (currentPlacementFile); if none is set it prompts once with a Save dialog.
    Private Sub SaveMission_Click(sender As Object, e As EventArgs)
        If Not String.IsNullOrEmpty(currentPlacementFile) AndAlso File.Exists(currentPlacementFile) Then
            If Not ReconcileWithDisk() Then Return
            SaveMissionToFile(currentPlacementFile)
        Else
            SaveMissionAs()
        End If
    End Sub

    ' Save the mission under a placement file chosen by the user; it becomes the open mission's file. A new file starts
    ' from the open one's text (its header and other tables come along), and the open mission's <Base>.lua is copied
    ' beside it when there is none, so the copy is a whole mission with its DLL.
    Private Sub SaveMissionAs()
        Using dlg As New SaveFileDialog()
            dlg.Title = "Save mission placement.lua"
            dlg.Filter = "Lua placement (*.lua)|*.lua|All files (*.*)|*.*"
            If Not String.IsNullOrEmpty(currentPlacementFile) Then
                dlg.InitialDirectory = Path.GetDirectoryName(currentPlacementFile)
                dlg.FileName = Path.GetFileName(currentPlacementFile)
            ElseIf Not String.IsNullOrEmpty(My.Settings.OP2Path) Then
                dlg.InitialDirectory = My.Settings.OP2Path
            End If
            If dlg.ShowDialog() <> DialogResult.OK Then Return
            Dim target As String = dlg.FileName
            If Not target.ToLowerInvariant().EndsWith(".placement.lua") Then
                target = Path.Combine(Path.GetDirectoryName(target), Path.GetFileNameWithoutExtension(target) & ".placement.lua")
            End If
            Try
                If Not File.Exists(target) AndAlso Not String.IsNullOrEmpty(currentPlacementFile) AndAlso File.Exists(currentPlacementFile) Then
                    File.Copy(currentPlacementFile, target)
                End If
                Dim script As String = Path.Combine(Path.GetDirectoryName(target), MissionDll.PlacementBase(target) & ".lua")
                If currentMissionScript IsNot Nothing AndAlso File.Exists(currentMissionScript) AndAlso Not File.Exists(script) Then
                    File.Copy(currentMissionScript, script)
                End If
                currentMissionScript = If(File.Exists(script), script, Nothing)
            Catch ex As Exception
                MessageBox.Show("Could not save mission: " & ex.Message, "Save Mission", MessageBoxButtons.OK, MessageBoxIcon.Error)
                Return
            End Try
            SaveMissionToFile(target)
            currentPlacementFile = target
            UpdateMissionLabel()
        End Using
    End Sub

    ' Writes the current regions + units tables into the placement file (inserting either block if it is absent),
    ' backing up the previous file as <file>.bak, then builds the mission DLL from the placement's header
    ' (MissionDll.SaveDll) so the DLL holds what the editor shows.
    Private Sub SaveMissionToFile(filePath As String)
        Dim summary As String
        Dim updated As String
        Try
            Dim existed As Boolean = File.Exists(filePath)
            Dim text As String = If(existed, File.ReadAllText(filePath), "return {" & vbCrLf & "}" & vbCrLf)
            updated = PlacementText.ReplaceTable(text, "regions", EmitRegionsBlock(PlacementText.NewLineOf(text)))
            updated = WriteUnits(updated, filePath)
            If existed Then File.Copy(filePath, filePath & ".bak", True)
            File.WriteAllText(filePath, updated)
            ReloadUnitsIfOpen(filePath, updated)
            summary = "Saved " & regionList.Count & " region(s) and " & unitList.Count & " unit(s) to " &
                      Path.GetFileName(filePath) & "." & If(existed, vbCrLf & "Backup: " & Path.GetFileName(filePath) & ".bak", "")
        Catch ex As Exception
            MessageBox.Show("Could not save mission: " & ex.Message, "Save Mission", MessageBoxButtons.OK, MessageBoxIcon.Error)
            Return
        End Try
        Try
            MessageBox.Show(summary & vbCrLf & vbCrLf & MissionDll.SaveDll(filePath, updated, FindBundledStub()),
                            "Save Mission", MessageBoxButtons.OK, MessageBoxIcon.Information)
        Catch ex As Exception
            MessageBox.Show(summary & vbCrLf & vbCrLf & "But the mission DLL was not written: " & ex.Message,
                            "Save Mission", MessageBoxButtons.OK, MessageBoxIcon.Warning)
        End Try
    End Sub

    ' ===== placement.lua I/O =====
    Private Sub LoadRegions_Click(sender As Object, e As EventArgs)
        Using dlg As New OpenFileDialog()
            dlg.Title = "Load regions from a placement.lua"
            dlg.Filter = "Lua placement (*.lua)|*.lua|All files (*.*)|*.*"
            If dlg.ShowDialog() <> DialogResult.OK Then Return
            Try
                Dim text As String = File.ReadAllText(dlg.FileName)
                LoadOverlay(text)                    ' regions, plus the units and beacons/walls/markers for context
                currentPlacementFile = dlg.FileName  ' so Save Mission / Ctrl+S targets this file
                currentMissionScript = Nothing
                UpdateMissionLabel()
                If mnuMissionProps IsNot Nothing Then mnuMissionProps.Enabled = True
                RefreshRegionList()
                RefreshUnitList()
                Dim panel As Control = FindControl("pnlMap") : If panel IsNot Nothing Then panel.Invalidate()
                MessageBox.Show("Loaded " & regionList.Count & " region(s) and " & unitList.Count & " unit(s).",
                                "Regions", MessageBoxButtons.OK, MessageBoxIcon.Information)
            Catch ex As Exception
                MessageBox.Show("Could not load: " & ex.Message, "Error", MessageBoxButtons.OK, MessageBoxIcon.Error)
            End Try
        End Using
    End Sub

    Private Sub SaveRegions_Click(sender As Object, e As EventArgs)
        Using dlg As New OpenFileDialog()
            dlg.Title = "Save regions into a placement.lua (its regions table is replaced)"
            dlg.Filter = "Lua placement (*.lua)|*.lua|All files (*.*)|*.*"
            If dlg.ShowDialog() <> DialogResult.OK Then Return
            Try
                Dim text As String = File.ReadAllText(dlg.FileName)
                Dim updated As String = PlacementText.ReplaceTable(text, "regions", EmitRegionsBlock(PlacementText.NewLineOf(text)))
                ' Back up the existing file first: <file>.bak (overwrites a previous backup).
                Dim backupPath As String = dlg.FileName & ".bak"
                File.Copy(dlg.FileName, backupPath, True)
                File.WriteAllText(dlg.FileName, updated)
                MessageBox.Show("Saved " & regionList.Count & " region(s) to " & Path.GetFileName(dlg.FileName) & "." &
                                vbCrLf & "Previous version backed up as " & Path.GetFileName(backupPath) & ".",
                                "Regions", MessageBoxButtons.OK, MessageBoxIcon.Information)
            Catch ex As Exception
                MessageBox.Show("Could not save regions: " & ex.Message, "Error", MessageBoxButtons.OK, MessageBoxIcon.Error)
            End Try
        End Using
    End Sub

    ' Emit the   regions = { ... }   table text (2-space indented, matching the templates), with the file's line ending.
    Private Function EmitRegionsBlock(nl As String) As String
        Dim sb As New StringBuilder()
        sb.Append("regions = {")
        sb.Append(nl)
        For Each r As MissionRegion In regionList
            If r.Kind = RegionKind.Point Then
                sb.Append("    " & r.Name & " = { " & r.X1 & ", " & r.Y1 & " },")
            Else
                sb.Append("    " & r.Name & " = { " & r.X1 & ", " & r.Y1 & ", " & r.X2 & ", " & r.Y2 & " },")
            End If
            sb.Append(nl)
        Next
        sb.Append("  }")
        Return sb.ToString()
    End Function

    ' Read the overlay (regions, units, beacons/walls/markers) from placement.lua text.
    Private Sub LoadOverlay(text As String)
        loadedPlacementText = text
        regionList = PlacementText.ParseRegions(text)
        unitsBlock = PlacementText.ParseUnits(text)
        mapObjects = PlacementText.ParseMapObjects(text)
        RefreshRegionList()
        RefreshUnitList()
    End Sub

    ' The units table written into a placement file's text. Saving into the file the units were read from also
    ' renumbers its beacons' `after = n` for the units deleted since (placement makes such a beacon after the n-th
    ' unit, so the units keep the object ids of the setup it remakes).
    Private Function WriteUnits(text As String, filePath As String) As String
        Dim updated As String = PlacementText.ReplaceTable(text, "units", PlacementText.EmitUnits(unitsBlock))
        If SamePath(filePath, currentPlacementFile) Then updated = PlacementText.FixBeaconAfter(updated, unitsBlock)
        Return updated
    End Function

    ' After saving the open mission, read its units back so the next save starts from what is in the file.
    Private Sub ReloadUnitsIfOpen(filePath As String, savedText As String)
        If Not SamePath(filePath, currentPlacementFile) Then Return
        Dim keep As Integer = If(lstUnits IsNot Nothing, lstUnits.SelectedIndex, -1)
        loadedPlacementText = savedText
        unitsBlock = PlacementText.ParseUnits(savedText)
        mapObjects = PlacementText.ParseMapObjects(savedText)
        RefreshUnitList()
        If lstUnits IsNot Nothing AndAlso keep >= 0 AndAlso keep < lstUnits.Items.Count Then lstUnits.SelectedIndex = keep
        InvalidateMap()
    End Sub

    ' Whether two paths name the same file (full paths, any case).
    Private Function SamePath(a As String, b As String) As Boolean
        If String.IsNullOrEmpty(a) OrElse String.IsNullOrEmpty(b) Then Return False
        Try
            Return String.Equals(Path.GetFullPath(a), Path.GetFullPath(b), StringComparison.OrdinalIgnoreCase)
        Catch
            Return False
        End Try
    End Function

    ' ", 12 beacon(s), 40 wall(s)/tube(s), 2 marker(s)" for the load messages ("" when there are none).
    Private Function MapObjectSummary() As String
        Dim b, w, m As Integer
        For Each o As MissionMapObject In mapObjects
            Select Case o.Kind
                Case MapObjectKind.Beacon : b += 1
                Case MapObjectKind.Wall : w += 1
                Case MapObjectKind.Marker : m += 1
            End Select
        Next
        Dim s As String = ""
        If b > 0 Then s &= ", " & b & " beacon(s)"
        If w > 0 Then s &= ", " & w & " wall(s)/tube(s)"
        If m > 0 Then s &= ", " & m & " marker(s)"
        Return s
    End Function

    ' Draw beacons (a diamond: C common, R rare, ? random ore, V magma vent, F fumarole), walls and tubes (a filled
    ' tile) and markers (a ring with the marker's name). tileRect gives a 0-based tile's rectangle on g.
    Private Sub DrawMapObjects(g As Graphics, tileRect As Func(Of Integer, Integer, System.Drawing.Rectangle), font As Font)
        If mapObjects Is Nothing Then Return
        For Each o As MissionMapObject In mapObjects
            If o.Kind <> MapObjectKind.Wall Then Continue For
            Dim col As Color
            Select Case o.ObjType
                Case "Tube" : col = tubeColor
                Case "LavaWall" : col = Color.FromArgb(190, 230, 90, 30)
                Case "MicrobeWall" : col = Color.FromArgb(190, 110, 160, 50)
                Case Else : col = Color.FromArgb(190, 120, 120, 120)
            End Select
            Using br As New SolidBrush(col)
                g.FillRectangle(br, tileRect(LuaToTile(o.X), LuaToTile(o.Y)))
            End Using
        Next
        For Each o As MissionMapObject In mapObjects
            If o.Kind = MapObjectKind.Wall Then Continue For
            Dim r As System.Drawing.Rectangle = tileRect(LuaToTile(o.X), LuaToTile(o.Y))
            Dim cx As Single = r.Left + r.Width / 2.0F
            Dim cy As Single = r.Top + r.Height / 2.0F
            Dim h As Single = Math.Max(3.0F, r.Width * 0.45F)
            If o.Kind = MapObjectKind.Beacon Then
                Dim label As String
                Dim col As Color
                Select Case o.ObjType
                    Case "MagmaVent" : label = "V" : col = Color.OrangeRed
                    Case "Fumarole" : label = "F" : col = Color.MediumPurple
                    Case Else
                        Select Case o.Ore
                            Case "Common" : label = "C" : col = Color.Gold
                            Case "Rare" : label = "R" : col = Color.DeepSkyBlue
                            Case Else : label = "?" : col = Color.WhiteSmoke
                        End Select
                End Select
                Dim pts As PointF() = {New PointF(cx, cy - h), New PointF(cx + h, cy), New PointF(cx, cy + h), New PointF(cx - h, cy)}
                Using br As New SolidBrush(Color.FromArgb(200, col))
                    g.FillPolygon(br, pts)
                End Using
                g.DrawPolygon(Pens.Black, pts)
                If r.Width >= 12 Then
                    Using f As New Font(font.FontFamily, Math.Max(6.0F, r.Width * 0.3F), FontStyle.Bold, GraphicsUnit.Pixel)
                        Dim sz As SizeF = g.MeasureString(label, f)
                        g.DrawString(label, f, Brushes.Black, cx - sz.Width / 2, cy - sz.Height / 2)
                    End Using
                End If
            Else
                Using pen As New Pen(Color.Magenta, Math.Max(1.0F, r.Width / 12.0F))
                    g.DrawEllipse(pen, cx - h, cy - h, 2 * h, 2 * h)
                End Using
                If Not String.IsNullOrEmpty(o.Name) Then g.DrawString(o.Name, font, Brushes.Magenta, r.Right, r.Top)
            End If
        Next
    End Sub

    ' Draw units as small player-coloured placeholder boxes with a short type label.
    Private Sub DrawUnits(g As Graphics)
        If Not showUnits OrElse unitList Is Nothing Then Return
        Dim sel As Integer = If(lstUnits IsNot Nothing, lstUnits.SelectedIndex, -1)
        For i As Integer = 0 To unitList.Count - 1
            Dim u As MissionUnit = unitList(i)
            If Not u.HasAt Then Continue For
            Dim t0x, t0y, t1x, t1y As Integer
            UnitFootprintTiles(u, t0x, t0y, t1x, t1y)
            Dim rect As System.Drawing.Rectangle = TilesToScreenRect(t0x, t0y, t1x, t1y)
            Dim col As Color = PlayerColor(u.Player)
            Using fill As New SolidBrush(Color.FromArgb(170, col))
                g.FillRectangle(fill, rect)
            End Using
            If i = sel Then
                Using selPen As New Pen(Color.White, 3)
                    g.DrawRectangle(selPen, rect)
                End Using
            Else
                g.DrawRectangle(Pens.Black, rect)
            End If

            Dim label As String = UnitAbbrev(u.UnitType)
            Dim f As Font = UnitLabelFont()
            Dim sz As SizeF = g.MeasureString(label, f)
            Dim lx As Single = rect.Left + (rect.Width - sz.Width) / 2
            Dim ly As Single = rect.Top + (rect.Height - sz.Height) / 2
            g.DrawString(label, f, Brushes.Black, lx + 1, ly + 1)
            g.DrawString(label, f, Brushes.White, lx, ly)
        Next
    End Sub

    ' The font of the unit labels on the map, made once (DrawUnits runs on every paint).
    Private unitLabelFontCache As Font = Nothing
    Private Function UnitLabelFont() As Font
        If unitLabelFontCache Is Nothing Then unitLabelFontCache = New Font(Me.Font.FontFamily, 7.0F, FontStyle.Bold)
        Return unitLabelFontCache
    End Function

    Private Shared ReadOnly unitAbbrevs As New Dictionary(Of String, String)(StringComparer.OrdinalIgnoreCase) From {
        {"CommandCenter", "CC"}, {"GuardPost", "GP"}, {"CargoTruck", "TRUCK"}, {"ConVec", "CONVEC"},
        {"Lynx", "LYNX"}, {"Tiger", "TIGER"}, {"Panther", "PANTH"}, {"Scorpion", "SCORP"},
        {"Tokamak", "TOK"}, {"MHDGenerator", "MHD"}, {"SolarPowerArray", "SOLAR"},
        {"StructureFactory", "SFAC"}, {"VehicleFactory", "VFAC"}, {"Agridome", "AGRI"},
        {"CommonOreSmelter", "SMELT"}, {"CommonOreMine", "MINE"}, {"Residence", "RES"},
        {"RoboMiner", "MINER"}, {"RoboSurveyor", "SURV"}, {"RoboDozer", "DOZER"}, {"Earthworker", "EWORK"},
        {"RareOreSmelter", "RSMELT"}, {"RareOreMine", "RMINE"}, {"CommonStorage", "STOR"}, {"RareStorage", "RSTOR"},
        {"GeothermalPlant", "GEO"}, {"RecreationFacility", "REC"}, {"ReinforcedResidence", "RRES"},
        {"AdvancedResidence", "ARES"}, {"EvacuationTransport", "EVAC"}, {"RepairVehicle", "REPAIR"},
        {"ArachnidFactory", "AFAC"}, {"ConsumerFactory", "CFAC"}, {"MedicalCenter", "MED"}, {"MeteorDefense", "METEOR"},
        {"LightTower", "LIGHT"}, {"StandardLab", "SLAB"}, {"AdvancedLab", "ALAB"}, {"BasicLab", "LAB"}}

    Private Function UnitAbbrev(unitType As String) As String
        If String.IsNullOrEmpty(unitType) Then Return "?"
        Dim ab As String = Nothing
        If unitAbbrevs.TryGetValue(unitType, ab) Then Return ab
        Return unitType.ToUpperInvariant()
    End Function

    ' Building footprints in tiles (X-size x Y-size), keyed by the .opm type name used in placement.lua.
    ' Source: the X-size / Y-size columns of OPU\base\sheets\building.txt. Vehicles and anything not listed here are
    ' 1x1. A structure's location is where its vertical and horizontal tubes would cross: its centre, or right of /
    ' below it for an even size (a Tokamak's is its lower-right tile).
    Private Shared ReadOnly buildingFootprints As New Dictionary(Of String, Size)(StringComparer.OrdinalIgnoreCase) From {
        {"CommandCenter", New Size(3, 2)}, {"RobotCommand", New Size(2, 2)}, {"TradeCenter", New Size(2, 2)},
        {"Tokamak", New Size(2, 2)}, {"MHDGenerator", New Size(2, 2)}, {"SolarPowerArray", New Size(3, 2)},
        {"GeothermalPlant", New Size(2, 1)},
        {"CommonOreMine", New Size(2, 1)}, {"RareOreMine", New Size(2, 1)}, {"MagmaWell", New Size(2, 1)},
        {"CommonOreSmelter", New Size(4, 3)}, {"RareOreSmelter", New Size(4, 3)},
        {"CommonStorage", New Size(1, 2)}, {"RareStorage", New Size(1, 2)},
        {"StructureFactory", New Size(4, 3)}, {"VehicleFactory", New Size(4, 3)}, {"ArachnidFactory", New Size(2, 2)},
        {"ConsumerFactory", New Size(3, 3)}, {"Garage", New Size(3, 2)}, {"Spaceport", New Size(5, 4)}, {"GORF", New Size(3, 2)},
        {"BasicLab", New Size(2, 2)}, {"StandardLab", New Size(3, 2)}, {"AdvancedLab", New Size(3, 3)}, {"Observatory", New Size(2, 2)},
        {"Residence", New Size(2, 2)}, {"ReinforcedResidence", New Size(3, 2)}, {"AdvancedResidence", New Size(3, 3)},
        {"Agridome", New Size(3, 2)}, {"Nursery", New Size(2, 2)}, {"University", New Size(2, 2)}, {"MedicalCenter", New Size(2, 2)},
        {"Forum", New Size(2, 2)}, {"RecreationFacility", New Size(2, 2)}, {"DIRT", New Size(3, 2)},
        {"MeteorDefense", New Size(2, 2)}, {"GuardPost", New Size(1, 1)}, {"LightTower", New Size(1, 1)},
        {"Tube", New Size(1, 1)}, {"Wall", New Size(1, 1)}, {"LavaWall", New Size(1, 1)}, {"MicrobeWall", New Size(1, 1)}}

    ' Tiles a player lays rather than buildings: no dozed ring, no tube connections of their own.
    Private Shared ReadOnly laidTypes As New HashSet(Of String)(StringComparer.OrdinalIgnoreCase) From {
        "Tube", "Wall", "LavaWall", "MicrobeWall"}

    ' Structures without tube connections (building.txt's Border column is N): power plants, mines, the magma well
    ' and light towers.
    Private Shared ReadOnly noTubeTypes As New HashSet(Of String)(StringComparer.OrdinalIgnoreCase) From {
        "Tokamak", "MHDGenerator", "GeothermalPlant", "SolarPowerArray", "LightTower",
        "MagmaWell", "CommonOreMine", "RareOreMine"}

    ' Whether the game clears a dozed border around this type: every structure (power plants and mines too), not
    ' tubes and walls.
    Private Function HasDozedBorder(unitType As String) As Boolean
        Return Not String.IsNullOrEmpty(unitType) AndAlso buildingFootprints.ContainsKey(unitType) AndAlso
               Not laidTypes.Contains(unitType)
    End Function

    ' Whether the structure connects to its colony by tubes (building.txt's Border column).
    Private Function HasTubes(unitType As String) As Boolean
        Return HasDozedBorder(unitType) AndAlso Not noTubeTypes.Contains(unitType)
    End Function

    ' The tube colour of the map overlay: the walls table's tubes and the ones structures lay alike.
    Private Shared ReadOnly tubeColor As Color = Color.FromArgb(170, 205, 170, 110)

    ' The two tubes the game lays when a structure with tube connections is placed: one tile east of its footprint on
    ' its location's row, one tile south of it on its location's column. Measured in the harness (every OPU 1.4.1
    ' structure placed by CreateUnit, the cells around it read back: op2lua/_work/sweep-tools/tubetest).
    ' tileRect gives a 0-based tile's rectangle on g.
    Private Sub DrawDroppedTubes(g As Graphics, tileRect As Func(Of Integer, Integer, System.Drawing.Rectangle))
        If unitList Is Nothing Then Return
        Using fill As New SolidBrush(tubeColor)
            For Each u As MissionUnit In unitList
                If Not u.HasAt OrElse Not HasTubes(u.UnitType) Then Continue For
                Dim t0x, t0y, t1x, t1y As Integer
                UnitFootprintTiles(u, t0x, t0y, t1x, t1y)
                g.FillRectangle(fill, tileRect(t1x + 1, LuaToTile(u.Y)))
                g.FillRectangle(fill, tileRect(LuaToTile(u.X), t1y + 1))
            Next
        End Using
    End Sub

    ' The bulldozed ring the game clears around a structure, one tile out from each structure's footprint,
    ' drawn under everything. A tile is dozed or not: tiles in two rings are collected once and filled once, so
    ' overlapping rings don't draw darker. tileRect gives a 0-based tile's rectangle on g.
    Private Sub DrawDozedBorders(g As Graphics, tileRect As Func(Of Integer, Integer, System.Drawing.Rectangle))
        If unitList Is Nothing Then Return
        Dim dozed As New HashSet(Of Integer)()
        For Each u As MissionUnit In unitList
            If Not u.HasAt OrElse Not HasDozedBorder(u.UnitType) Then Continue For
            Dim t0x, t0y, t1x, t1y As Integer
            UnitFootprintTiles(u, t0x, t0y, t1x, t1y)
            For x As Integer = t0x - 1 To t1x + 1
                For y As Integer = t0y - 1 To t1y + 1
                    dozed.Add((x + 1000) * 10000 + (y + 1000))      ' one key per tile (off-map tiles too)
                Next
            Next
        Next
        Using fill As New SolidBrush(Color.FromArgb(150, 72, 66, 60))
            For Each k As Integer In dozed
                g.FillRectangle(fill, tileRect(k \ 10000 - 1000, k Mod 10000 - 1000))
            Next
        End Using
    End Sub

    ' Footprint (tiles) for a type. Unknown / vehicles -> 1x1.
    Private Function FootprintOf(unitType As String) As Size
        Dim s As Size
        If Not String.IsNullOrEmpty(unitType) AndAlso buildingFootprints.TryGetValue(unitType, s) Then Return s
        Return New Size(1, 1)
    End Function

    ' 0-based map-tile rectangle a unit occupies. The placement coord (u.X,u.Y) is the footprint CENTRE
    ' (OP2: top-left = centre - size\2), so 1x1 units sit exactly on their tile.
    Private Sub UnitFootprintTiles(u As MissionUnit, ByRef t0x As Integer, ByRef t0y As Integer, ByRef t1x As Integer, ByRef t1y As Integer)
        Dim fp As Size = FootprintOf(u.UnitType)
        Dim cx As Integer = LuaToTile(u.X)
        Dim cy As Integer = LuaToTile(u.Y)
        t0x = cx - fp.Width \ 2
        t0y = cy - fp.Height \ 2
        t1x = t0x + fp.Width - 1
        t1y = t0y + fp.Height - 1
    End Sub

    Private Function PlayerColor(player As Integer) As Color
        Select Case player
            Case 1 : Return Color.DodgerBlue
            Case 2 : Return Color.Red
            Case 3 : Return Color.LimeGreen
            Case 4 : Return Color.Gold
            Case 5 : Return Color.Orange
            Case 6 : Return Color.Magenta
            Case Else : Return Color.Silver
        End Select
    End Function

    ' ===== unit editing (place / move / delete + type/player/weapon pickers + save) =====
    Private Sub SetupUnitsUI()
        Dim unitsMenu As New ToolStripMenuItem("Units")
        unitsMenu.Enabled = False       ' enabled once a map is loaded
        unitsMenuTop = unitsMenu

        mnuUnitMode = New ToolStripMenuItem("Unit Place Mode")
        mnuUnitMode.CheckOnClick = True
        AddHandler mnuUnitMode.Click, AddressOf UnitMode_Click
        unitsMenu.DropDownItems.Add(mnuUnitMode)
        unitsMenu.DropDownItems.Add(New ToolStripSeparator())

        unitsMenu.DropDownItems.Add(BuildGroupedChoiceMenu("Unit Type",
                                                           {Tuple.Create("Vehicles", vehicleTypeChoices), Tuple.Create("Structures", structureTypeChoices)},
                                                           currentUnitType, Sub(v) currentUnitType = v))
        unitsMenu.DropDownItems.Add(BuildChoiceMenu("Player", New String() {"1", "2", "3", "4", "5", "6"},
                                                    currentUnitPlayer.ToString(), Sub(v) currentUnitPlayer = Integer.Parse(v)))
        unitsMenu.DropDownItems.Add(BuildChoiceMenu("Weapon", unitWeaponChoices,
                                                    If(currentUnitWeapon = "", "(none)", currentUnitWeapon),
                                                    Sub(v) currentUnitWeapon = If(v = "(none)", "", v)))
        unitsMenu.DropDownItems.Add(New ToolStripSeparator())

        Dim mSave As New ToolStripMenuItem("Save Units to placement.lua...")
        AddHandler mSave.Click, AddressOf SaveUnits_Click
        unitsMenu.DropDownItems.Add(mSave)
        Dim mDel As New ToolStripMenuItem("Delete Selected Unit")
        AddHandler mDel.Click, AddressOf DeleteUnit_Click
        unitsMenu.DropDownItems.Add(mDel)
        Dim mClr As New ToolStripMenuItem("Clear All Units")
        AddHandler mClr.Click, AddressOf ClearUnits_Click
        unitsMenu.DropDownItems.Add(mClr)

        If MainMenuStrip IsNot Nothing Then
            MainMenuStrip.Items.Insert(Math.Max(0, MainMenuStrip.Items.Count - 1), unitsMenu)
        End If

        lblUnitsHeader = ListHeader("Units")
        lstUnits = New ListBox() With {.Name = "lstUnits", .Dock = DockStyle.Fill, .IntegralHeight = False,
                                       .Font = ListFont(), .HorizontalScrollbar = True}
        AddHandler lstUnits.SelectedIndexChanged, Sub(s, ev) InvalidateMap()
        Dim ctx As New ContextMenuStrip()
        Dim ctxDel As New ToolStripMenuItem("Delete Unit")
        AddHandler ctxDel.Click, AddressOf DeleteUnit_Click
        ctx.Items.Add(ctxDel)
        lstUnits.ContextMenuStrip = ctx
        AddHandler lstUnits.MouseDown, Sub(s, ev)
                                           If ev.Button = MouseButtons.Right Then
                                               Dim idx As Integer = lstUnits.IndexFromPoint(ev.Location)
                                               If idx >= 0 Then lstUnits.SelectedIndex = idx
                                           End If
                                       End Sub
        listsSplit.Panel2.Controls.Add(lstUnits)
        listsSplit.Panel2.Controls.Add(lblUnitsHeader)
        lstUnits.BringToFront()
        SizeListsSplit(GetPropertiesPanel())
        RefreshUnitList()
    End Sub

    ' A submenu of mutually-exclusive checkable choices; onSelect fires with the chosen value.
    Private Function BuildChoiceMenu(title As String, choices As String(), current As String, onSelect As Action(Of String)) As ToolStripMenuItem
        Dim root As New ToolStripMenuItem(title & ":  " & current)
        For Each c As String In choices
            Dim item As New ToolStripMenuItem(c)
            item.Checked = (c = current)
            Dim val As String = c
            AddHandler item.Click, Sub(s, ev)
                                       onSelect(val)
                                       For Each sib As ToolStripItem In root.DropDownItems
                                           If TypeOf sib Is ToolStripMenuItem Then CType(sib, ToolStripMenuItem).Checked = (sib Is item)
                                       Next
                                       root.Text = title & ":  " & val
                                   End Sub
            root.DropDownItems.Add(item)
        Next
        Return root
    End Function

    ' A choice menu whose choices sit in named submenus (Vehicles / Structures); one choice is checked across them.
    Private Function BuildGroupedChoiceMenu(title As String, groups As Tuple(Of String, String())(), current As String,
                                            onSelect As Action(Of String)) As ToolStripMenuItem
        Dim root As New ToolStripMenuItem(title & ":  " & current)
        Dim all As New List(Of ToolStripMenuItem)()
        For Each grp As Tuple(Of String, String()) In groups
            Dim sub_ As New ToolStripMenuItem(grp.Item1)
            For Each c As String In grp.Item2
                Dim item As New ToolStripMenuItem(c)
                item.Checked = (c = current)
                Dim val As String = c
                AddHandler item.Click, Sub(s, ev)
                                           onSelect(val)
                                           For Each other As ToolStripMenuItem In all
                                               other.Checked = (other Is item)
                                           Next
                                           root.Text = title & ":  " & val
                                       End Sub
                all.Add(item)
                sub_.DropDownItems.Add(item)
            Next
            root.DropDownItems.Add(sub_)
        Next
        Return root
    End Function

    Private Sub UnitMode_Click(sender As Object, e As EventArgs)
        unitMode = mnuUnitMode.Checked
        If unitMode AndAlso mnuRegionMode IsNot Nothing Then
            mnuRegionMode.Checked = False : regionMode = False
        End If
        UpdateMapCursor()
    End Sub

    ' ----- mouse (called from the map panel handlers when unitMode is on) -----
    Public Function UnitMouseDown(e As MouseEventArgs) As Boolean
        If Not unitMode OrElse currentMap Is Nothing Then Return False
        If e.Button <> MouseButtons.Left Then Return False
        Dim tx As Integer = MouseToTileX(e.X)
        Dim ty As Integer = MouseToTileY(e.Y)
        If tx < 0 OrElse ty < 0 OrElse tx >= currentMap.WidthInTiles() OrElse ty >= currentMap.HeightInTiles() Then Return True

        Dim hit As Integer = UnitAtTile(tx, ty)
        If hit >= 0 Then                          ' grab an existing unit to move it
            lstUnits.SelectedIndex = hit
            unitDragging = True
            unitDragIndex = hit
            InvalidateMap()
            Return True
        End If

        Dim u As New MissionUnit()                ' empty tile -> place a new unit
        u.UnitType = currentUnitType
        u.Player = currentUnitPlayer
        u.Weapon = currentUnitWeapon
        u.X = TileToLua(tx)
        u.Y = TileToLua(ty)
        unitList.Add(u)
        RefreshUnitList()
        lstUnits.SelectedIndex = unitList.Count - 1
        InvalidateMap()
        Return True
    End Function

    ' In unit mode: drag the grabbed unit to the tile under the cursor.
    Public Sub UnitMouseMove(e As MouseEventArgs)
        If Not unitMode OrElse Not unitDragging Then Return
        If unitDragIndex < 0 OrElse unitDragIndex >= unitList.Count Then Return
        unitList(unitDragIndex).X = TileToLua(MouseToTileX(e.X))
        unitList(unitDragIndex).Y = TileToLua(MouseToTileY(e.Y))
        InvalidateMap()
    End Sub

    ' In unit mode: drop the dragged unit.
    Public Sub UnitMouseUp(e As MouseEventArgs)
        If Not unitMode OrElse Not unitDragging Then Return
        unitDragging = False
        RefreshUnitList()
        InvalidateMap()
    End Sub

    ' The index of the topmost unit on the map whose footprint covers 0-based tile (tx, ty), or -1.
    Private Function UnitAtTile(tx As Integer, ty As Integer) As Integer
        For i As Integer = unitList.Count - 1 To 0 Step -1
            If Not unitList(i).HasAt Then Continue For
            Dim t0x, t0y, t1x, t1y As Integer
            UnitFootprintTiles(unitList(i), t0x, t0y, t1x, t1y)
            If tx >= t0x AndAlso tx <= t1x AndAlso ty >= t0y AndAlso ty <= t1y Then Return i
        Next
        Return -1
    End Function

    ' Fill the units list (and its header count) from unitList, keeping the selection.
    Private Sub RefreshUnitList()
        If lblUnitsHeader IsNot Nothing Then
            lblUnitsHeader.Text = "Units (" & unitList.Count & ")   place mode: click = add, drag = move"
        End If
        If lstUnits Is Nothing Then Return
        Dim keep As Integer = lstUnits.SelectedIndex
        lstUnits.BeginUpdate()
        lstUnits.Items.Clear()
        For Each u As MissionUnit In unitList
            lstUnits.Items.Add(u.ToString())
        Next
        lstUnits.EndUpdate()
        If keep >= 0 AndAlso keep < lstUnits.Items.Count Then lstUnits.SelectedIndex = keep
    End Sub

    ' Delete the selected unit (PlacementText.DeleteUnit keeps a heading comment above it).
    Private Sub DeleteUnit_Click(sender As Object, e As EventArgs)
        Dim i As Integer = If(lstUnits IsNot Nothing, lstUnits.SelectedIndex, -1)
        If i < 0 OrElse i >= unitList.Count Then Return
        PlacementText.DeleteUnit(unitsBlock, i)
        RefreshUnitList()
        InvalidateMap()
    End Sub

    ' Delete every unit, last first, so the comments between them are handled as for single deletes.
    Private Sub ClearUnits_Click(sender As Object, e As EventArgs)
        If unitList.Count = 0 Then Return
        If MessageBox.Show("Clear all units?", "Confirm", MessageBoxButtons.YesNo, MessageBoxIcon.Question) = DialogResult.Yes Then
            While unitList.Count > 0
                PlacementText.DeleteUnit(unitsBlock, unitList.Count - 1)
            End While
            RefreshUnitList()
            InvalidateMap()
        End If
    End Sub

    ' Write the units table into a placement file the user picks (its units table is replaced, the rest kept).
    Private Sub SaveUnits_Click(sender As Object, e As EventArgs)
        Using dlg As New OpenFileDialog()
            dlg.Title = "Save units into a placement.lua (its units table is replaced)"
            dlg.Filter = "Lua placement (*.lua)|*.lua|All files (*.*)|*.*"
            If dlg.ShowDialog() <> DialogResult.OK Then Return
            Try
                Dim text As String = File.ReadAllText(dlg.FileName)
                Dim updated As String = WriteUnits(text, dlg.FileName)
                File.Copy(dlg.FileName, dlg.FileName & ".bak", True)
                File.WriteAllText(dlg.FileName, updated)
                ReloadUnitsIfOpen(dlg.FileName, updated)
                MessageBox.Show("Saved " & unitList.Count & " unit(s)." & vbCrLf &
                                "Previous version backed up as " & Path.GetFileName(dlg.FileName) & ".bak.",
                                "Units", MessageBoxButtons.OK, MessageBoxIcon.Information)
            Catch ex As Exception
                MessageBox.Show("Could not save units: " & ex.Message, "Error", MessageBoxButtons.OK, MessageBoxIcon.Error)
            End Try
        End Using
    End Sub

    ' Draw regions (+ units) in MAP-PIXEL space (32 px per tile, no zoom/pan) for image export.
    Private Sub RenderOverlayMapPixels(g As Graphics)
        g.SmoothingMode = Drawing2D.SmoothingMode.AntiAlias
        Using regionFont As New Font(Me.Font.FontFamily, 16.0F, FontStyle.Bold),
              unitFont As New Font(Me.Font.FontFamily, 9.0F, FontStyle.Bold)
            ' regions
            For i As Integer = 0 To regionList.Count - 1
                Dim r As MissionRegion = regionList(i)
                Dim col As Color = regionColors(i Mod regionColors.Length)
                Dim ax As Integer = LuaToTile(Math.Min(r.X1, r.X2)) * 32
                Dim ay As Integer = LuaToTile(Math.Min(r.Y1, r.Y2)) * 32
                Dim bw As Integer = (Math.Abs(r.X2 - r.X1) + 1) * 32
                Dim bh As Integer = (Math.Abs(r.Y2 - r.Y1) + 1) * 32
                Dim rect As New System.Drawing.Rectangle(ax, ay, bw, bh)
                Using fill As New SolidBrush(Color.FromArgb(70, col))
                    g.FillRectangle(fill, rect)
                End Using
                Using pen As New Pen(col, 4)
                    g.DrawRectangle(pen, rect)
                End Using
                If r.Kind = RegionKind.Point Then
                    Using cross As New Pen(col, 2)
                        g.DrawLine(cross, rect.Left, rect.Top, rect.Right, rect.Bottom)
                        g.DrawLine(cross, rect.Right, rect.Top, rect.Left, rect.Bottom)
                    End Using
                End If
                Dim sz As SizeF = g.MeasureString(r.Name, regionFont)
                Using bg As New SolidBrush(Color.FromArgb(190, Color.Black))
                    g.FillRectangle(bg, rect.Left, rect.Top - sz.Height, sz.Width + 6, sz.Height)
                End Using
                Using nameBrush As New SolidBrush(col)
                    g.DrawString(r.Name, regionFont, nameBrush, rect.Left + 3, rect.Top - sz.Height)
                End Using
            Next
            ' beacons, walls and markers, then units
            If showUnits Then DrawDozedBorders(g, Function(tx, ty) New System.Drawing.Rectangle(tx * 32, ty * 32, 32, 32))
            If showUnits Then DrawDroppedTubes(g, Function(tx, ty) New System.Drawing.Rectangle(tx * 32, ty * 32, 32, 32))
            If showMapObjects Then DrawMapObjects(g, Function(tx, ty) New System.Drawing.Rectangle(tx * 32, ty * 32, 32, 32), unitFont)
            If showUnits AndAlso unitList IsNot Nothing Then
                For Each u As MissionUnit In unitList
                    If Not u.HasAt Then Continue For
                    Dim t0x, t0y, t1x, t1y As Integer
                    UnitFootprintTiles(u, t0x, t0y, t1x, t1y)
                    Dim rect As New System.Drawing.Rectangle(t0x * 32, t0y * 32, (t1x - t0x + 1) * 32, (t1y - t0y + 1) * 32)
                    Using fill As New SolidBrush(Color.FromArgb(200, PlayerColor(u.Player)))
                        g.FillRectangle(fill, rect)
                    End Using
                    g.DrawRectangle(Pens.Black, rect)
                    Dim label As String = UnitAbbrev(u.UnitType)
                    Dim sz As SizeF = g.MeasureString(label, unitFont)
                    Dim lx As Single = rect.Left + (rect.Width - sz.Width) / 2
                    Dim ly As Single = rect.Top + (rect.Height - sz.Height) / 2
                    g.DrawString(label, unitFont, Brushes.Black, lx + 1, ly + 1)
                    g.DrawString(label, unitFont, Brushes.White, lx, ly)
                Next
            End If
        End Using
    End Sub

End Class
