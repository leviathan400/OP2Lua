' fMain.Regions.vb
'
' Visual region editing for OP2Lua missions, layered onto the OP2MapViewer form via a partial class.
' Regions are the backbone of Lua mission logic (region:contains, spawn at = regions[...], trip-wires).
'
' You draw rectangles / points on the map; they are saved into a placement.lua "regions" table and
' can be loaded back and drawn on the map (round-trip). Coordinates are 1-based tile coords - the
' same numbers the status bar shows and that go straight into placement.lua.
'
' Hooks in fMain.vb (kept tiny):
'   fMain_Load            -> SetupRegionsUI()
'   MapPanel_Paint        -> DrawRegions(e.Graphics)
'   MapPanel_MouseDown    -> If regionMode AndAlso RegionMouseDown(e) Then Return
'   MapPanel_MouseMove    -> If regionMode Then RegionMouseMove(e)
'   MapPanel_MouseUp      -> If regionMode Then RegionMouseUp(e)

Imports System.Drawing
Imports System.IO
Imports System.Text
Imports System.Text.RegularExpressions
Imports System.Windows.Forms

Partial Public Class fMain

    ' Region/unit data types now live in MissionTypes.vb (top-level, so the Form designer is happy).

    ' ---- state ----
    Private regionList As New List(Of MissionRegion)
    Private unitList As New List(Of MissionUnit)
    Private showUnits As Boolean = True
    Private regionMode As Boolean = False
    Private regionDrawing As Boolean = False
    Private regionStartTX As Integer, regionStartTY As Integer       ' 0-based tile during drag
    Private regionCurTX As Integer, regionCurTY As Integer
    Private lstRegions As ListBox = Nothing
    Private mnuRegionMode As ToolStripMenuItem = Nothing
    Private mnuShowUnits As ToolStripMenuItem = Nothing
    Private regionsMenuTop As ToolStripMenuItem = Nothing    ' top-level "Regions" menu (disabled until a map is loaded)
    Private unitsMenuTop As ToolStripMenuItem = Nothing      ' top-level "Units" menu (disabled until a map is loaded)
    Private missionMenuTop As ToolStripMenuItem = Nothing    ' top-level "Mission" menu (always enabled)
    Private mnuMissionProps As ToolStripMenuItem = Nothing   ' "Properties..." (enabled once a mission is open)
    Private mnuSaveMission As ToolStripMenuItem = Nothing     ' "Save Mission" (enabled once a map is loaded)

    ' ---- unit editing state ----
    Private unitMode As Boolean = False
    Private currentUnitType As String = "Lynx"
    Private currentUnitPlayer As Integer = 1
    Private currentUnitWeapon As String = ""          ' "" = none
    Private lstUnits As ListBox = Nothing
    Private mnuUnitMode As ToolStripMenuItem = Nothing
    Private unitDragging As Boolean = False
    Private unitDragIndex As Integer = -1

    Private ReadOnly unitTypeChoices As String() = New String() {
        "Lynx", "Panther", "Tiger", "Scorpion", "ConVec", "CargoTruck", "RoboMiner", "RoboSurveyor",
        "RoboDozer", "Earthworker", "Scout", "Spider", "RepairVehicle", "EvacuationTransport",
        "CommandCenter", "Tokamak", "MHDGenerator", "SolarPowerArray", "Agridome", "StructureFactory",
        "VehicleFactory", "GuardPost", "CommonOreSmelter", "CommonOreMine", "Tube", "Wall"}
    Private ReadOnly unitWeaponChoices As String() = New String() {
        "(none)", "Laser", "Microwave", "RailGun", "RPG", "EMP", "Stickyfoam", "ThorsHammer",
        "ESG", "AcidCloud", "Starflare", "Supernova"}

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

        ' --- regions list, docked at the bottom of the properties panel ---
        Dim props As Control = GetPropertiesPanel()
        Dim lbl As New Label()
        lbl.Text = "Regions (draw mode: drag = area, click = point)"
        lbl.Dock = DockStyle.Bottom
        lbl.Height = 18
        lbl.TextAlign = ContentAlignment.MiddleLeft

        lstRegions = New ListBox()
        lstRegions.Name = "lstRegions"
        lstRegions.Dock = DockStyle.Bottom
        lstRegions.Height = 150
        lstRegions.IntegralHeight = False
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

        props.Controls.Add(lstRegions)
        props.Controls.Add(lbl)

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
                g.DrawLine(New Pen(col, 1), rect.Left, rect.Top, rect.Right, rect.Bottom)
                g.DrawLine(New Pen(col, 1), rect.Right, rect.Top, rect.Left, rect.Bottom)
            End If

            ' name label
            Using bg As New SolidBrush(Color.FromArgb(180, Color.Black))
                Dim sz As SizeF = g.MeasureString(r.Name, Me.Font)
                g.FillRectangle(bg, rect.Left, rect.Top - sz.Height, sz.Width + 4, sz.Height)
                g.DrawString(r.Name, Me.Font, New SolidBrush(col), rect.Left + 2, rect.Top - sz.Height)
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

        ' units (placeholders parsed from placement.lua)
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
    Private Function TilesToScreenRect(t0x As Integer, t0y As Integer, t1x As Integer, t1y As Integer) As System.Drawing.Rectangle
        Dim sx As Integer = CInt(t0x * 32 * zoomLevel) + offsetX
        Dim sy As Integer = CInt(t0y * 32 * zoomLevel) + offsetY
        Dim w As Integer = CInt((t1x - t0x + 1) * 32 * zoomLevel)
        Dim h As Integer = CInt((t1y - t0y + 1) * 32 * zoomLevel)
        Return New System.Drawing.Rectangle(sx, sy, w, h)
    End Function

    ' ===== list management =====
    ' Clear all placed regions and units (called by Close Map) and refresh both list boxes.
    Public Sub ClearMissionData()
        regionList.Clear()
        unitList.Clear()
        RefreshRegionList()
        RefreshUnitList()
    End Sub

    Private Sub RefreshRegionList()
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

    ' ===== Mission Properties (edit placement.lua metadata + keep the DLL in sync) =====
    ' The metadata (name/map/tech/type/max_tech/players_count) lives at the top of placement.lua, but
    ' the NAME and MAP are ALSO baked into the mission DLL as exports OP2 reads directly (LevelDesc /
    ' MapName), before Lua runs. So changing the map means: rewrite placement.lua AND patch <Base>.dll.
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

        Dim oldName As String = ScalarString(text, "name")
        Dim oldMap As String = ScalarString(text, "map")
        Dim oldTech As String = ScalarString(text, "tech")
        Dim oldType As String = ScalarString(text, "type")
        Dim oldMaxTech As Integer = ScalarInt(text, "max_tech", 12)
        Dim oldPlayers As Integer = ScalarInt(text, "players_count", 2)

        Using f As New fMissionProps()
            f.MissionName = oldName
            f.MapFile = oldMap
            f.Tech = If(String.IsNullOrEmpty(oldTech), "MULTITEK.TXT", oldTech)
            f.MissionTypeName = If(String.IsNullOrEmpty(oldType), "Colony", oldType)
            f.MaxTech = oldMaxTech
            f.PlayersCount = oldPlayers
            f.MapBrowseDir = MapBrowseRoot()
            If f.ShowDialog(Me) <> DialogResult.OK Then Return

            ' Round-trip the scalar metadata back into the placement text (first occurrence of each key).
            Dim updated As String = text
            updated = ReplaceScalarString(updated, "name", f.MissionName)
            updated = ReplaceScalarString(updated, "map", f.MapFile)
            updated = ReplaceScalarString(updated, "tech", f.Tech)
            updated = ReplaceScalarString(updated, "type", f.MissionTypeName)
            updated = ReplaceScalarInt(updated, "max_tech", f.MaxTech)
            updated = ReplaceScalarInt(updated, "players_count", f.PlayersCount)

            Try
                File.Copy(currentPlacementFile, currentPlacementFile & ".bak", True)
                File.WriteAllText(currentPlacementFile, updated)
            Catch ex As Exception
                MessageBox.Show("Could not save placement file: " & ex.Message, "Mission Properties", MessageBoxButtons.OK, MessageBoxIcon.Error)
                Return
            End Try

            ' Keep the sibling <Base>.dll's name/map exports in sync (OP2 reads these directly).
            Dim baseName As String = Path.GetFileName(currentPlacementFile)
            If baseName.ToLowerInvariant().EndsWith(".placement.lua") Then baseName = baseName.Substring(0, baseName.Length - ".placement.lua".Length)
            Dim dllPath As String = Path.Combine(Path.GetDirectoryName(currentPlacementFile), baseName & ".dll")
            Dim dllNote As String = ""
            If File.Exists(dllPath) Then
                Try
                    Dim bytes() As Byte = File.ReadAllBytes(dllPath)
                    Dim patched As Integer = 0
                    Dim failed As New List(Of String)()
                    If f.MissionName <> oldName Then
                        If PatchDllAsciiField(bytes, oldName, f.MissionName) Then patched += 1 Else failed.Add("name")
                        PatchDllWideField(bytes, oldName, f.MissionName)   ' keep FileDescription in sync (best-effort)
                    End If
                    If f.MapFile <> oldMap Then
                        If PatchDllAsciiField(bytes, oldMap, f.MapFile) Then patched += 1 Else failed.Add("map")
                    End If
                    If patched > 0 Then
                        File.Copy(dllPath, dllPath & ".bak", True)
                        File.WriteAllBytes(dllPath, bytes)
                    End If
                    If patched > 0 OrElse failed.Count > 0 Then
                        dllNote = vbCrLf & vbCrLf & baseName & ".dll: patched " & patched & " export(s)" &
                                  If(failed.Count > 0, "; could NOT locate: " & String.Join(", ", failed) & " (DLL left unchanged for those).", ".")
                    End If
                Catch ex As Exception
                    dllNote = vbCrLf & vbCrLf & "Warning: could not patch " & baseName & ".dll: " & ex.Message
                End Try
            ElseIf f.MapFile <> oldMap OrElse f.MissionName <> oldName Then
                dllNote = vbCrLf & vbCrLf & "Note: no " & baseName & ".dll beside the placement file. The map/name are also baked into the " &
                          "mission DLL (OP2 reads them from it), so they won't change in-game until the DLL is built/patched."
            End If

            ' If the map changed, reload the new map onto the canvas (overlay regions/units unchanged).
            If f.MapFile <> oldMap Then
                Dim newMapPath As String = ResolveMapPath(f.MapFile, Path.GetDirectoryName(currentPlacementFile))
                If String.IsNullOrEmpty(newMapPath) Then
                    dllNote &= vbCrLf & vbCrLf & "(Could not find """ & f.MapFile & """ to display - it will still apply in-game.)"
                ElseIf LoadMapFile(newMapPath) Then
                    RefreshRegionList()
                    RefreshUnitList()
                    Dim panel As Control = FindControl("pnlMap") : If panel IsNot Nothing Then panel.Invalidate()
                End If
            End If

            Text = ApplicationName & " - " & f.MissionName

            MessageBox.Show("Mission properties saved to " & Path.GetFileName(currentPlacementFile) & "." & dllNote,
                            "Mission Properties", MessageBoxButtons.OK, MessageBoxIcon.Information)
        End Using
    End Sub

    ' --- placement.lua scalar metadata helpers (first occurrence = the top-level metadata) ---

    Private Function ScalarString(text As String, key As String) As String
        Dim m As Match = Regex.Match(text, "\b" & Regex.Escape(key) & "\s*=\s*""([^""]*)""")
        Return If(m.Success, m.Groups(1).Value, "")
    End Function

    Private Function ScalarInt(text As String, key As String, fallback As Integer) As Integer
        Dim m As Match = Regex.Match(text, "\b" & Regex.Escape(key) & "\s*=\s*(-?\d+)")
        Dim v As Integer
        If m.Success AndAlso Integer.TryParse(m.Groups(1).Value, v) Then Return v
        Return fallback
    End Function

    Private Function ReplaceScalarString(text As String, key As String, value As String) As String
        Dim rx As New Regex("(\b" & Regex.Escape(key) & "\s*=\s*"")[^""]*("")")
        If Not rx.IsMatch(text) Then Return text
        Return rx.Replace(text, "${1}" & value.Replace("$", "$$") & "${2}", 1)
    End Function

    Private Function ReplaceScalarInt(text As String, key As String, value As Integer) As String
        Dim rx As New Regex("(\b" & Regex.Escape(key) & "\s*=\s*)-?\d+")
        If Not rx.IsMatch(text) Then Return text
        Return rx.Replace(text, "${1}" & value.ToString(), 1)
    End Function

    ' Folder the "Change Map..." dialog should open in: <OP2>\OPU (falls back to the OP2 path).
    Private Function MapBrowseRoot() As String
        Dim base As String = My.Settings.OP2Path
        If String.IsNullOrEmpty(base) Then Return ""
        Dim opu As String = Path.Combine(base, "OPU")
        Return If(Directory.Exists(opu), opu, base)
    End Function

    ' --- mission DLL byte-patching (mirrors op2lua-newmission's patchField / patchWideField) ---

    ' The stub's FileDescription placeholder (UTF-16 in the version resource). Must stay byte-for-byte
    ' identical to the FileDescription in stub/LuaMission.rc and OP2LUA_DESC_PLACEHOLDER in the CLI tools.
    Private Const StubDescPlaceholder As String = "OP2Lua mission name placeholder - set by the new-mission tool"

    ' Overwrite a fixed char[1024] export string in a mission DLL: find a NUL-terminated occurrence of
    ' oldStr (its current value), zero the field, write newStr + NUL. Returns False if not found.
    Private Function PatchDllAsciiField(buf() As Byte, oldStr As String, newStr As String) As Boolean
        If String.IsNullOrEmpty(oldStr) Then Return False
        Dim oldBytes() As Byte = System.Text.Encoding.ASCII.GetBytes(oldStr)
        Dim newBytes() As Byte = System.Text.Encoding.ASCII.GetBytes(newStr)
        If newBytes.Length + 1 >= 1024 Then Return False

        ' Find an occurrence immediately followed by a NUL (i.e. a complete field value, not a substring).
        Dim idx As Integer = -1
        Dim from As Integer = 0
        Do
            Dim hit As Integer = IndexOfBytes(buf, oldBytes, from)
            If hit < 0 Then Exit Do
            Dim term As Integer = hit + oldBytes.Length
            If term < buf.Length AndAlso buf(term) = 0 Then idx = hit : Exit Do
            from = hit + 1
        Loop
        If idx < 0 Then Return False

        Dim clear As Integer = Math.Max(oldBytes.Length, newBytes.Length + 1)
        For k As Integer = 0 To clear - 1
            If idx + k < buf.Length Then buf(idx + k) = 0
        Next
        For k As Integer = 0 To newBytes.Length - 1
            buf(idx + k) = newBytes(k)
        Next
        Return True
    End Function

    ' Overwrite a UTF-16 string value in the DLL's version resource (FileDescription): find a
    ' NUL-terminated occurrence of oldStr (UTF-16LE) and replace it with newStr, zero-filling the rest
    ' of the field. Length-preserving within the field's existing capacity (oldStr plus the run of NUL
    ' words after it); returns False if not found or newStr won't fit. Mirrors patchWideField.
    Private Function PatchDllWideField(buf() As Byte, oldStr As String, newStr As String) As Boolean
        If String.IsNullOrEmpty(oldStr) Then Return False
        Dim needle() As Byte = System.Text.Encoding.Unicode.GetBytes(oldStr)   ' UTF-16LE

        ' Find an occurrence followed by a UTF-16 NUL word (0x0000) - a complete field value.
        Dim idx As Integer = -1
        Dim from As Integer = 0
        Do
            Dim hit As Integer = IndexOfBytes(buf, needle, from)
            If hit < 0 Then Exit Do
            Dim term As Integer = hit + needle.Length
            If term + 1 < buf.Length AndAlso buf(term) = 0 AndAlso buf(term + 1) = 0 Then idx = hit : Exit Do
            from = hit + 1
        Loop
        If idx < 0 Then Return False

        ' Field capacity in WORDs = old chars + the trailing run of NUL words (stops at the next
        ' non-zero byte, i.e. the following resource structure). Reserve one word for the terminator.
        Dim capWords As Integer = oldStr.Length
        Dim p As Integer = idx + needle.Length
        Do While p + 1 < buf.Length AndAlso buf(p) = 0 AndAlso buf(p + 1) = 0
            capWords += 1
            p += 2
        Loop
        If newStr.Length > capWords - 1 Then Return False

        Dim regionBytes As Integer = capWords * 2
        For k As Integer = 0 To regionBytes - 1
            If idx + k < buf.Length Then buf(idx + k) = 0
        Next
        Dim newBytes() As Byte = System.Text.Encoding.Unicode.GetBytes(newStr)
        For k As Integer = 0 To newBytes.Length - 1
            buf(idx + k) = newBytes(k)
        Next
        Return True
    End Function

    ' First index of needle within haystack at/after start (-1 if none).
    Private Function IndexOfBytes(haystack() As Byte, needle() As Byte, start As Integer) As Integer
        If needle.Length = 0 OrElse haystack.Length < needle.Length Then Return -1
        Dim last As Integer = haystack.Length - needle.Length
        For i As Integer = start To last
            Dim ok As Boolean = True
            For j As Integer = 0 To needle.Length - 1
                If haystack(i + j) <> needle(j) Then ok = False : Exit For
            Next
            If ok Then Return i
        Next
        Return -1
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

            ' Work out the placement file regardless of which sibling was picked.
            Dim picked As String = dlg.FileName
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
                Return
            End If

            If Not LoadMission(placementFile) Then Return

            Dim scriptNote As String = If(currentMissionScript Is Nothing, "(no matching mission .lua found)", Path.GetFileName(currentMissionScript))
            MessageBox.Show("Mission loaded." & vbCrLf & vbCrLf &
                            "Script:    " & scriptNote & vbCrLf &
                            "Placement: " & Path.GetFileName(placementFile) & vbCrLf &
                            "Map:       " & lastLoadedMapName & vbCrLf & vbCrLf &
                            regionList.Count & " region(s), " & unitList.Count & " unit(s).",
                            "Open Mission", MessageBoxButtons.OK, MessageBoxIcon.Information)
        End Using
    End Sub

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
        lastLoadedMapName = mapName
        If mnuMissionProps IsNot Nothing Then mnuMissionProps.Enabled = True

        regionList = ParseRegions(placementText)
        unitList = ParseUnits(placementText)
        RefreshRegionList()
        RefreshUnitList()

        ' Title the window after the mission (placement details.name), not the .map LoadMapFile set.
        Dim missionName As String = ScalarString(placementText, "name")
        If String.IsNullOrEmpty(missionName) Then missionName = Path.GetFileNameWithoutExtension(placementFile)
        Text = ApplicationName & " - " & missionName

        Dim panel As Control = FindControl("pnlMap") : If panel IsNot Nothing Then panel.Invalidate()
        Return True
    End Function

    ' ===== New Mission (scaffold a fresh mission: clone+patch the bundled stub, write starter lua) =====
    ' Mirrors op2lua-newmission: clones the bundled LuaMission.dll, byte-patches its menu name + map,
    ' writes starter <Base>.lua + <Base>.placement.lua, into  <OP2>\OPU\maps\<Base>\ , then opens it.
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
            f.MissionName = ""
            f.MapFile = "eden01.map"
            f.Tech = "MULTITEK.TXT"
            f.MissionTypeName = "Colony"
            f.MaxTech = 12
            f.PlayersCount = 2
            f.MapBrowseDir = MapBrowseRoot()
            If f.ShowDialog(Me) <> DialogResult.OK Then Return

            Dim base As String = BaseNameFrom(f.MissionName)
            If String.IsNullOrEmpty(base) Then
                MessageBox.Show("The mission name needs at least one letter or digit.", "New Mission", MessageBoxButtons.OK, MessageBoxIcon.Warning)
                Return
            End If

            ' OP2 1.4.1 loads missions flat from OPU\maps\LUA - write the three files straight in there.
            Dim outDir As String = Path.Combine(My.Settings.OP2Path, "OPU", "maps", "LUA")
            Dim dllOut As String = Path.Combine(outDir, base & ".dll")
            Dim placementOut As String = Path.Combine(outDir, base & ".placement.lua")
            Dim scriptOut As String = Path.Combine(outDir, base & ".lua")

            If File.Exists(dllOut) OrElse File.Exists(placementOut) OrElse File.Exists(scriptOut) Then
                If MessageBox.Show("A mission already exists in:" & vbCrLf & outDir & vbCrLf & vbCrLf & "Overwrite its files?",
                                   "New Mission", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) <> DialogResult.Yes Then Return
            End If

            Try
                Directory.CreateDirectory(outDir)

                ' DLL: clone the stub, then byte-patch the menu name + map exports + FileDescription.
                Dim bytes() As Byte = File.ReadAllBytes(stubPath)
                If Not PatchDllAsciiField(bytes, "OP2Lua mission (unpatched template)", f.MissionName) Then
                    MessageBox.Show("The bundled stub does not contain the expected name placeholder - it may be the wrong DLL.",
                                    "New Mission", MessageBoxButtons.OK, MessageBoxIcon.Error)
                    Return
                End If
                PatchDllAsciiField(bytes, "eden01.map", f.MapFile)
                PatchDllWideField(bytes, StubDescPlaceholder, f.MissionName)   ' FileDescription (Properties > Details)
                File.WriteAllBytes(dllOut, bytes)

                ' Starter lua files.
                File.WriteAllText(placementOut, NewPlacementSkeleton(base, f.MissionName, f.MapFile, f.Tech, f.MissionTypeName, f.MaxTech, f.PlayersCount))
                File.WriteAllText(scriptOut, NewMissionSkeleton(base, f.MissionName))
            Catch ex As Exception
                MessageBox.Show("Could not create mission: " & ex.Message, "New Mission", MessageBoxButtons.OK, MessageBoxIcon.Error)
                Return
            End Try

            ' Open the new mission straight away.
            If LoadMission(placementOut) Then
                Text = ApplicationName & " - " & f.MissionName
                MessageBox.Show("Created mission """ & f.MissionName & """ in:" & vbCrLf & outDir & vbCrLf & vbCrLf &
                                base & ".dll   (menu name + map patched)" & vbCrLf &
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

    ' Locate the bundled stub DLL (copied next to the editor exe at build time).
    Private Function FindBundledStub() As String
        Dim exeDir As String = Application.StartupPath
        For Each c As String In New String() {
            Path.Combine(exeDir, "stub", "LuaMission.dll"),
            Path.Combine(exeDir, "LuaMission.dll")}
            If File.Exists(c) Then Return c
        Next
        Return Nothing
    End Function

    Private Function NewPlacementSkeleton(base As String, name As String, map As String, tech As String,
                                          missionType As String, maxTech As Integer, players As Integer) As String
        Dim q As Func(Of String, String) = Function(s) """" & s & """"
        Dim sb As New StringBuilder()
        sb.AppendLine("-- " & base & ".placement.lua  -  " & name & "  -  starting layout.")
        sb.AppendLine("-- Coordinates are the ones shown on the in-game status bar (hover a tile to read them).")
        sb.AppendLine("return {")
        sb.AppendLine("  name = " & q(name) & ", map = " & q(map) & ", tech = " & q(tech) & ", type = " & q(missionType) & ",")
        sb.AppendLine("  max_tech = " & maxTech & ", players_count = " & players & ",")
        sb.AppendLine("")
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

    Private Function NewMissionSkeleton(base As String, name As String) As String
        Dim sb As New StringBuilder()
        sb.AppendLine("-- " & base & ".lua  -  " & name & "  -  mission logic.")
        sb.AppendLine("-- See docs/API.md for the full API. Edit freely; this is your mission.")
        sb.AppendLine("")
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
            SaveMissionToFile(currentPlacementFile)
        Else
            SaveMissionAs()
        End If
    End Sub

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
            SaveMissionToFile(dlg.FileName)
            currentPlacementFile = dlg.FileName
        End Using
    End Sub

    ' Writes the current regions + units tables into the placement file (inserting either block if it
    ' is absent), backing up the previous file as <file>.bak.
    Private Sub SaveMissionToFile(filePath As String)
        Try
            Dim existed As Boolean = File.Exists(filePath)
            Dim text As String = If(existed, File.ReadAllText(filePath), "return {" & vbCrLf & "}" & vbCrLf)
            Dim updated As String = ReplaceRegionsBlock(text, EmitRegionsBlock())
            updated = ReplaceUnitsBlock(updated, EmitUnitsBlock())
            If existed Then File.Copy(filePath, filePath & ".bak", True)
            File.WriteAllText(filePath, updated)
            MessageBox.Show("Saved " & regionList.Count & " region(s) and " & unitList.Count & " unit(s) to " &
                            Path.GetFileName(filePath) & "." & If(existed, vbCrLf & "Backup: " & Path.GetFileName(filePath) & ".bak", ""),
                            "Save Mission", MessageBoxButtons.OK, MessageBoxIcon.Information)
        Catch ex As Exception
            MessageBox.Show("Could not save mission: " & ex.Message, "Save Mission", MessageBoxButtons.OK, MessageBoxIcon.Error)
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
                regionList = ParseRegions(text)
                unitList = ParseUnits(text)          ' also pull units for context
                currentPlacementFile = dlg.FileName  ' so Save Mission / Ctrl+S targets this file
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
                Dim updated As String = ReplaceRegionsBlock(text, EmitRegionsBlock())
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

    ' Emit the   regions = { ... }   table text (2-space indented, matching the templates).
    Private Function EmitRegionsBlock() As String
        Dim sb As New StringBuilder()
        sb.Append("regions = {")
        sb.Append(vbCrLf)
        For Each r As MissionRegion In regionList
            If r.Kind = RegionKind.Point Then
                sb.Append("    " & r.Name & " = { " & r.X1 & ", " & r.Y1 & " },")
            Else
                sb.Append("    " & r.Name & " = { " & r.X1 & ", " & r.Y1 & ", " & r.X2 & ", " & r.Y2 & " },")
            End If
            sb.Append(vbCrLf)
        Next
        sb.Append("  }")
        Return sb.ToString()
    End Function

    ' Replace an existing   regions = { ... }   block with newBlock. If none exists, insert before the
    ' final closing brace of the returned table.
    Private Function ReplaceRegionsBlock(text As String, newBlock As String) As String
        Dim m As Match = Regex.Match(text, "regions\s*=\s*\{")
        If m.Success Then
            Dim braceOpen As Integer = text.IndexOf("{"c, m.Index)
            Dim closeIdx As Integer = MatchBrace(text, braceOpen)
            If closeIdx > braceOpen Then
                Return text.Substring(0, m.Index) & newBlock & text.Substring(closeIdx + 1)
            End If
        End If
        ' no regions block: insert before the last '}' in the file
        Dim lastBrace As Integer = text.LastIndexOf("}"c)
        If lastBrace < 0 Then Return text & vbCrLf & newBlock & vbCrLf
        Return text.Substring(0, lastBrace) & "  " & newBlock & "," & vbCrLf & text.Substring(lastBrace)
    End Function

    ' Index of the '}' matching the '{' at openIndex (-1 if unbalanced).
    Private Function MatchBrace(text As String, openIndex As Integer) As Integer
        Dim depth As Integer = 0
        For i As Integer = openIndex To text.Length - 1
            Dim c As Char = text(i)
            If c = "{"c Then
                depth += 1
            ElseIf c = "}"c Then
                depth -= 1
                If depth = 0 Then Return i
            End If
        Next
        Return -1
    End Function

    ' Parse   name = { x, y }   (point) and   name = { x1, y1, x2, y2 }   (rect) from a regions block.
    Private Function ParseRegions(text As String) As List(Of MissionRegion)
        Dim result As New List(Of MissionRegion)()
        Dim m As Match = Regex.Match(text, "regions\s*=\s*\{")
        If Not m.Success Then Return result
        Dim braceOpen As Integer = text.IndexOf("{"c, m.Index)
        Dim closeIdx As Integer = MatchBrace(text, braceOpen)
        If closeIdx <= braceOpen Then Return result
        Dim inner As String = text.Substring(braceOpen + 1, closeIdx - braceOpen - 1)

        Dim entry As New Regex("([A-Za-z_]\w*)\s*=\s*\{\s*(-?\d+)\s*,\s*(-?\d+)\s*(?:,\s*(-?\d+)\s*,\s*(-?\d+)\s*)?\}")
        For Each em As Match In entry.Matches(inner)
            Dim r As New MissionRegion()
            r.Name = em.Groups(1).Value
            r.X1 = Integer.Parse(em.Groups(2).Value)
            r.Y1 = Integer.Parse(em.Groups(3).Value)
            If em.Groups(4).Success Then
                r.Kind = RegionKind.Rectangle
                r.X2 = Integer.Parse(em.Groups(4).Value)
                r.Y2 = Integer.Parse(em.Groups(5).Value)
            Else
                r.Kind = RegionKind.Point
                r.X2 = r.X1 : r.Y2 = r.Y1
            End If
            result.Add(r)
        Next
        Return result
    End Function

    ' ===== units (placeholders parsed from the placement.lua "units" table) =====

    ' Parse the units array: each entry has type="X", at={x,y}, optional player=N, name="..."
    Private Function ParseUnits(text As String) As List(Of MissionUnit)
        Dim result As New List(Of MissionUnit)()
        Dim m As Match = Regex.Match(text, "units\s*=\s*\{")
        If Not m.Success Then Return result
        Dim braceOpen As Integer = text.IndexOf("{"c, m.Index)
        Dim closeIdx As Integer = MatchBrace(text, braceOpen)
        If closeIdx <= braceOpen Then Return result
        Dim inner As String = text.Substring(braceOpen + 1, closeIdx - braceOpen - 1)

        ' walk depth-1 { ... } entries (each is one unit table)
        Dim depth As Integer = 0
        Dim start As Integer = -1
        For i As Integer = 0 To inner.Length - 1
            Dim c As Char = inner(i)
            If c = "{"c Then
                depth += 1
                If depth = 1 Then start = i + 1
            ElseIf c = "}"c Then
                If depth = 1 AndAlso start >= 0 Then
                    Dim u As MissionUnit = ParseUnitEntry(inner.Substring(start, i - start))
                    If u IsNot Nothing Then result.Add(u)
                End If
                depth -= 1
            End If
        Next
        Return result
    End Function

    Private Function ParseUnitEntry(entry As String) As MissionUnit
        Dim atM As Match = Regex.Match(entry, "at\s*=\s*\{\s*(-?\d+)\s*,\s*(-?\d+)")
        If Not atM.Success Then Return Nothing                     ' no position -> skip
        Dim u As New MissionUnit()
        u.X = Integer.Parse(atM.Groups(1).Value)
        u.Y = Integer.Parse(atM.Groups(2).Value)
        Dim tM As Match = Regex.Match(entry, "type\s*=\s*""([^""]+)""")
        u.UnitType = If(tM.Success, tM.Groups(1).Value, "Unit")
        Dim pM As Match = Regex.Match(entry, "player\s*=\s*(\d+)")
        u.Player = If(pM.Success, Integer.Parse(pM.Groups(1).Value), 1)
        Dim nM As Match = Regex.Match(entry, "name\s*=\s*""([^""]+)""")
        If nM.Success Then u.Name = nM.Groups(1).Value
        Dim wM As Match = Regex.Match(entry, "weapon\s*=\s*""([^""]+)""")
        If wM.Success Then u.Weapon = wM.Groups(1).Value
        Dim cM As Match = Regex.Match(entry, "cargo\s*=\s*""([^""]+)""")
        If cM.Success Then u.Cargo = cM.Groups(1).Value
        Dim aM As Match = Regex.Match(entry, "amount\s*=\s*(\d+)")
        If aM.Success Then u.Amount = Integer.Parse(aM.Groups(1).Value)
        Return u
    End Function

    ' Draw units as small player-coloured placeholder boxes with a short type label.
    Private Sub DrawUnits(g As Graphics)
        If Not showUnits OrElse unitList Is Nothing Then Return
        Dim sel As Integer = If(lstUnits IsNot Nothing, lstUnits.SelectedIndex, -1)
        For i As Integer = 0 To unitList.Count - 1
            Dim u As MissionUnit = unitList(i)
            Dim t0x, t0y, t1x, t1y As Integer
            UnitFootprintTiles(u, t0x, t0y, t1x, t1y)
            Dim rect As System.Drawing.Rectangle = TilesToScreenRect(t0x, t0y, t1x, t1y)
            Dim col As Color = PlayerColor(u.Player)
            Using fill As New SolidBrush(Color.FromArgb(170, col))
                g.FillRectangle(fill, rect)
            End Using
            If i = sel Then
                g.DrawRectangle(New Pen(Color.White, 3), rect)
            Else
                g.DrawRectangle(Pens.Black, rect)
            End If

            Dim label As String = UnitAbbrev(u.UnitType)
            Using f As New Font(Me.Font.FontFamily, 7.0F, FontStyle.Bold)
                Dim sz As SizeF = g.MeasureString(label, f)
                Dim lx As Single = rect.Left + (rect.Width - sz.Width) / 2
                Dim ly As Single = rect.Top + (rect.Height - sz.Height) / 2
                g.DrawString(label, f, Brushes.Black, lx + 1, ly + 1)
                g.DrawString(label, f, Brushes.White, lx, ly)
            End Using
        Next
    End Sub

    Private Shared ReadOnly unitAbbrevs As New Dictionary(Of String, String)(StringComparer.OrdinalIgnoreCase) From {
        {"CommandCenter", "CC"}, {"GuardPost", "GP"}, {"CargoTruck", "TRUCK"}, {"ConVec", "CONVEC"},
        {"Lynx", "LYNX"}, {"Tiger", "TIGER"}, {"Panther", "PANTH"}, {"Scorpion", "SCORP"},
        {"Tokamak", "TOK"}, {"MHDGenerator", "MHD"}, {"SolarPowerArray", "SOLAR"},
        {"StructureFactory", "SFAC"}, {"VehicleFactory", "VFAC"}, {"Agridome", "AGRI"},
        {"CommonOreSmelter", "SMELT"}, {"CommonOreMine", "MINE"}, {"ResidenceuD", "RES"},
        {"RoboMiner", "MINER"}, {"RoboSurveyor", "SURV"}, {"RoboDozer", "DOZER"}, {"Earthworker", "EWORK"}}

    Private Function UnitAbbrev(unitType As String) As String
        If String.IsNullOrEmpty(unitType) Then Return "?"
        Dim ab As String = Nothing
        If unitAbbrevs.TryGetValue(unitType, ab) Then Return ab
        Return unitType.ToUpperInvariant()
    End Function

    ' Building footprints in tiles (X-size x Y-size), keyed by the .opm type name used in placement.lua.
    ' Source: OPU\base\sheets\building.txt (see D:\opu\op2remake\OP2_BUILDINGS.md). Vehicles and anything
    ' not listed here are 1x1. OP2 stores a structure's position as its footprint CENTRE.
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
        {"Forum", New Size(2, 2)}, {"RecFacility", New Size(2, 2)}, {"DIRT", New Size(3, 2)},
        {"MeteorDefense", New Size(2, 2)}, {"GuardPost", New Size(1, 1)}, {"LightTower", New Size(1, 1)},
        {"Tube", New Size(1, 1)}, {"Wall", New Size(1, 1)}, {"LavaWall", New Size(1, 1)}, {"MicrobeWall", New Size(1, 1)}}

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

        unitsMenu.DropDownItems.Add(BuildChoiceMenu("Unit Type", unitTypeChoices, currentUnitType, Sub(v) currentUnitType = v))
        unitsMenu.DropDownItems.Add(BuildChoiceMenu("Player", New String() {"1", "2", "3", "4"},
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

        Dim props As Control = GetPropertiesPanel()
        Dim lbl As New Label() With {.Text = "Units (place mode: click = add, drag = move)",
                                     .Dock = DockStyle.Bottom, .Height = 18, .TextAlign = ContentAlignment.MiddleLeft}
        lstUnits = New ListBox() With {.Name = "lstUnits", .Dock = DockStyle.Bottom, .Height = 120, .IntegralHeight = False}
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
        props.Controls.Add(lstUnits)
        props.Controls.Add(lbl)
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

    Public Sub UnitMouseMove(e As MouseEventArgs)
        If Not unitMode OrElse Not unitDragging Then Return
        If unitDragIndex < 0 OrElse unitDragIndex >= unitList.Count Then Return
        unitList(unitDragIndex).X = TileToLua(MouseToTileX(e.X))
        unitList(unitDragIndex).Y = TileToLua(MouseToTileY(e.Y))
        InvalidateMap()
    End Sub

    Public Sub UnitMouseUp(e As MouseEventArgs)
        If Not unitMode OrElse Not unitDragging Then Return
        unitDragging = False
        RefreshUnitList()
        InvalidateMap()
    End Sub

    Private Function UnitAtTile(tx As Integer, ty As Integer) As Integer
        For i As Integer = unitList.Count - 1 To 0 Step -1
            Dim t0x, t0y, t1x, t1y As Integer
            UnitFootprintTiles(unitList(i), t0x, t0y, t1x, t1y)
            If tx >= t0x AndAlso tx <= t1x AndAlso ty >= t0y AndAlso ty <= t1y Then Return i
        Next
        Return -1
    End Function

    Private Sub RefreshUnitList()
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

    Private Sub DeleteUnit_Click(sender As Object, e As EventArgs)
        Dim i As Integer = If(lstUnits IsNot Nothing, lstUnits.SelectedIndex, -1)
        If i < 0 OrElse i >= unitList.Count Then Return
        unitList.RemoveAt(i)
        RefreshUnitList()
        InvalidateMap()
    End Sub

    Private Sub ClearUnits_Click(sender As Object, e As EventArgs)
        If unitList.Count = 0 Then Return
        If MessageBox.Show("Clear all units?", "Confirm", MessageBoxButtons.YesNo, MessageBoxIcon.Question) = DialogResult.Yes Then
            unitList.Clear()
            RefreshUnitList()
            InvalidateMap()
        End If
    End Sub

    Private Sub SaveUnits_Click(sender As Object, e As EventArgs)
        Using dlg As New OpenFileDialog()
            dlg.Title = "Save units into a placement.lua (its units table is replaced)"
            dlg.Filter = "Lua placement (*.lua)|*.lua|All files (*.*)|*.*"
            If dlg.ShowDialog() <> DialogResult.OK Then Return
            Try
                Dim text As String = File.ReadAllText(dlg.FileName)
                Dim updated As String = ReplaceUnitsBlock(text, EmitUnitsBlock())
                File.Copy(dlg.FileName, dlg.FileName & ".bak", True)
                File.WriteAllText(dlg.FileName, updated)
                MessageBox.Show("Saved " & unitList.Count & " unit(s)." & vbCrLf &
                                "Previous version backed up as " & Path.GetFileName(dlg.FileName) & ".bak.",
                                "Units", MessageBoxButtons.OK, MessageBoxIcon.Information)
            Catch ex As Exception
                MessageBox.Show("Could not save units: " & ex.Message, "Error", MessageBoxButtons.OK, MessageBoxIcon.Error)
            End Try
        End Using
    End Sub

    ' Emit the   units = { ... }   table. Preserves name/weapon/cargo captured on load.
    Private Function EmitUnitsBlock() As String
        Dim sb As New StringBuilder()
        sb.Append("units = {")
        sb.Append(vbCrLf)
        For Each u As MissionUnit In unitList
            sb.Append("    { ")
            If Not String.IsNullOrEmpty(u.Name) Then sb.Append("name = """ & u.Name & """, ")
            sb.Append("type = """ & u.UnitType & """, player = " & u.Player & ", at = { " & u.X & ", " & u.Y & " }")
            If Not String.IsNullOrEmpty(u.Weapon) Then sb.Append(", weapon = """ & u.Weapon & """")
            If Not String.IsNullOrEmpty(u.Cargo) Then sb.Append(", cargo = """ & u.Cargo & """, amount = " & u.Amount)
            sb.Append(" },")
            sb.Append(vbCrLf)
        Next
        sb.Append("  }")
        Return sb.ToString()
    End Function

    Private Function ReplaceUnitsBlock(text As String, newBlock As String) As String
        Dim m As Match = Regex.Match(text, "units\s*=\s*\{")
        If m.Success Then
            Dim braceOpen As Integer = text.IndexOf("{"c, m.Index)
            Dim closeIdx As Integer = MatchBrace(text, braceOpen)
            If closeIdx > braceOpen Then
                Return text.Substring(0, m.Index) & newBlock & text.Substring(closeIdx + 1)
            End If
        End If
        Dim lastBrace As Integer = text.LastIndexOf("}"c)
        If lastBrace < 0 Then Return text & vbCrLf & newBlock & vbCrLf
        Return text.Substring(0, lastBrace) & "  " & newBlock & "," & vbCrLf & text.Substring(lastBrace)
    End Function

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
                    g.DrawLine(New Pen(col, 2), rect.Left, rect.Top, rect.Right, rect.Bottom)
                    g.DrawLine(New Pen(col, 2), rect.Right, rect.Top, rect.Left, rect.Bottom)
                End If
                Dim sz As SizeF = g.MeasureString(r.Name, regionFont)
                Using bg As New SolidBrush(Color.FromArgb(190, Color.Black))
                    g.FillRectangle(bg, rect.Left, rect.Top - sz.Height, sz.Width + 6, sz.Height)
                End Using
                g.DrawString(r.Name, regionFont, New SolidBrush(col), rect.Left + 3, rect.Top - sz.Height)
            Next
            ' units
            If showUnits AndAlso unitList IsNot Nothing Then
                For Each u As MissionUnit In unitList
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
