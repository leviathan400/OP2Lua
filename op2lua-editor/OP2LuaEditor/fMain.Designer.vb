<Global.Microsoft.VisualBasic.CompilerServices.DesignerGenerated()> _
Partial Class fMain
    Inherits System.Windows.Forms.Form

    'Form overrides dispose to clean up the component list.
    <System.Diagnostics.DebuggerNonUserCode()> _
    Protected Overrides Sub Dispose(ByVal disposing As Boolean)
        Try
            If disposing AndAlso components IsNot Nothing Then
                components.Dispose()
            End If
        Finally
            MyBase.Dispose(disposing)
        End Try
    End Sub

    'Required by the Windows Form Designer
    Private components As System.ComponentModel.IContainer

    'NOTE: The following procedure is required by the Windows Form Designer
    'It can be modified using the Windows Form Designer.  
    'Do not modify it using the code editor.
    <System.Diagnostics.DebuggerStepThrough()> _
    Private Sub InitializeComponent()
        Me.pnlProperties = New System.Windows.Forms.Panel()
        Me.lblMapImage = New System.Windows.Forms.Label()
        Me.lblTileCount = New System.Windows.Forms.Label()
        Me.lblMapSize = New System.Windows.Forms.Label()
        Me.lblZoomLevel = New System.Windows.Forms.Label()
        Me.lblCellType = New System.Windows.Forms.Label()
        Me.lblCoordinates = New System.Windows.Forms.Label()
        Me.lblMappingIndex = New System.Windows.Forms.Label()
        Me.lblTilesetImage = New System.Windows.Forms.Label()
        Me.lblTileset = New System.Windows.Forms.Label()
        Me.lblMapName = New System.Windows.Forms.Label()
        Me.pnlMap = New System.Windows.Forms.Panel()
        Me.pnlProperties.SuspendLayout()
        Me.SuspendLayout()
        '
        'pnlProperties
        '
        Me.pnlProperties.Controls.Add(Me.lblMapImage)
        Me.pnlProperties.Controls.Add(Me.lblTileCount)
        Me.pnlProperties.Controls.Add(Me.lblMapSize)
        Me.pnlProperties.Controls.Add(Me.lblZoomLevel)
        Me.pnlProperties.Controls.Add(Me.lblCellType)
        Me.pnlProperties.Controls.Add(Me.lblCoordinates)
        Me.pnlProperties.Controls.Add(Me.lblMappingIndex)
        Me.pnlProperties.Controls.Add(Me.lblTilesetImage)
        Me.pnlProperties.Controls.Add(Me.lblTileset)
        Me.pnlProperties.Controls.Add(Me.lblMapName)
        Me.pnlProperties.Location = New System.Drawing.Point(624, 12)
        Me.pnlProperties.Name = "pnlProperties"
        Me.pnlProperties.Size = New System.Drawing.Size(254, 511)
        Me.pnlProperties.TabIndex = 1
        '
        'lblMapImage
        '
        Me.lblMapImage.AutoSize = True
        Me.lblMapImage.Location = New System.Drawing.Point(22, 50)
        Me.lblMapImage.Name = "lblMapImage"
        Me.lblMapImage.Size = New System.Drawing.Size(67, 13)
        Me.lblMapImage.TabIndex = 10
        Me.lblMapImage.Text = "lblMapImage"
        '
        'lblTileCount
        '
        Me.lblTileCount.AutoSize = True
        Me.lblTileCount.Location = New System.Drawing.Point(22, 63)
        Me.lblTileCount.Name = "lblTileCount"
        Me.lblTileCount.Size = New System.Drawing.Size(62, 13)
        Me.lblTileCount.TabIndex = 9
        Me.lblTileCount.Text = "lblTileCount"
        '
        'lblMapSize
        '
        Me.lblMapSize.AutoSize = True
        Me.lblMapSize.Location = New System.Drawing.Point(22, 37)
        Me.lblMapSize.Name = "lblMapSize"
        Me.lblMapSize.Size = New System.Drawing.Size(58, 13)
        Me.lblMapSize.TabIndex = 8
        Me.lblMapSize.Text = "lblMapSize"
        '
        'lblZoomLevel
        '
        Me.lblZoomLevel.AutoSize = True
        Me.lblZoomLevel.Location = New System.Drawing.Point(22, 185)
        Me.lblZoomLevel.Name = "lblZoomLevel"
        Me.lblZoomLevel.Size = New System.Drawing.Size(70, 13)
        Me.lblZoomLevel.TabIndex = 7
        Me.lblZoomLevel.Text = "lblZoomLevel"
        '
        'lblCellType
        '
        Me.lblCellType.AutoSize = True
        Me.lblCellType.Location = New System.Drawing.Point(22, 154)
        Me.lblCellType.Name = "lblCellType"
        Me.lblCellType.Size = New System.Drawing.Size(58, 13)
        Me.lblCellType.TabIndex = 5
        Me.lblCellType.Text = "lblCellType"
        '
        'lblCoordinates
        '
        Me.lblCoordinates.AutoSize = True
        Me.lblCoordinates.Location = New System.Drawing.Point(22, 141)
        Me.lblCoordinates.Name = "lblCoordinates"
        Me.lblCoordinates.Size = New System.Drawing.Size(73, 13)
        Me.lblCoordinates.TabIndex = 4
        Me.lblCoordinates.Text = "lblCoordinates"
        '
        'lblMappingIndex
        '
        Me.lblMappingIndex.AutoSize = True
        Me.lblMappingIndex.Location = New System.Drawing.Point(22, 128)
        Me.lblMappingIndex.Name = "lblMappingIndex"
        Me.lblMappingIndex.Size = New System.Drawing.Size(84, 13)
        Me.lblMappingIndex.TabIndex = 3
        Me.lblMappingIndex.Text = "lblMappingIndex"
        '
        'lblTilesetImage
        '
        Me.lblTilesetImage.AutoSize = True
        Me.lblTilesetImage.Location = New System.Drawing.Point(22, 115)
        Me.lblTilesetImage.Name = "lblTilesetImage"
        Me.lblTilesetImage.Size = New System.Drawing.Size(77, 13)
        Me.lblTilesetImage.TabIndex = 2
        Me.lblTilesetImage.Text = "lblTilesetImage"
        '
        'lblTileset
        '
        Me.lblTileset.AutoSize = True
        Me.lblTileset.Location = New System.Drawing.Point(22, 102)
        Me.lblTileset.Name = "lblTileset"
        Me.lblTileset.Size = New System.Drawing.Size(48, 13)
        Me.lblTileset.TabIndex = 1
        Me.lblTileset.Text = "lblTileset"
        '
        'lblMapName
        '
        Me.lblMapName.AutoSize = True
        Me.lblMapName.Location = New System.Drawing.Point(22, 24)
        Me.lblMapName.Name = "lblMapName"
        Me.lblMapName.Size = New System.Drawing.Size(66, 13)
        Me.lblMapName.TabIndex = 0
        Me.lblMapName.Text = "lblMapName"
        '
        'pnlMap
        '
        Me.pnlMap.AllowDrop = True
        Me.pnlMap.Location = New System.Drawing.Point(12, 12)
        Me.pnlMap.Name = "pnlMap"
        Me.pnlMap.Size = New System.Drawing.Size(450, 511)
        Me.pnlMap.TabIndex = 2
        '
        'fMain
        '
        Me.AutoScaleDimensions = New System.Drawing.SizeF(6.0!, 13.0!)
        Me.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font
        Me.ClientSize = New System.Drawing.Size(908, 613)
        Me.Controls.Add(Me.pnlMap)
        Me.Controls.Add(Me.pnlProperties)
        Me.Name = "fMain"
        Me.Text = "Outpost 2 Lua Editor"
        Me.pnlProperties.ResumeLayout(False)
        Me.pnlProperties.PerformLayout()
        Me.ResumeLayout(False)

    End Sub
    Friend WithEvents pnlProperties As Panel
    Friend WithEvents lblCellType As Label
    Friend WithEvents lblCoordinates As Label
    Friend WithEvents lblMappingIndex As Label
    Friend WithEvents lblTilesetImage As Label
    Friend WithEvents lblTileset As Label
    Friend WithEvents lblMapName As Label
    Friend WithEvents lblZoomLevel As Label
    Friend WithEvents lblMapSize As Label
    Friend WithEvents lblTileCount As Label
    Friend WithEvents pnlMap As Panel
    Friend WithEvents lblMapImage As Label
End Class
