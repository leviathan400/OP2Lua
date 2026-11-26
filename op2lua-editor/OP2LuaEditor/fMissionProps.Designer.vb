<Global.Microsoft.VisualBasic.CompilerServices.DesignerGenerated()>
Partial Class fMissionProps
    Inherits System.Windows.Forms.Form

    'Form overrides dispose to clean up the component list.
    <System.Diagnostics.DebuggerNonUserCode()>
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
    <System.Diagnostics.DebuggerStepThrough()>
    Private Sub InitializeComponent()
        Me.lblName = New System.Windows.Forms.Label()
        Me.txtName = New System.Windows.Forms.TextBox()
        Me.lblMap = New System.Windows.Forms.Label()
        Me.txtMap = New System.Windows.Forms.TextBox()
        Me.btnMap = New System.Windows.Forms.Button()
        Me.lblTech = New System.Windows.Forms.Label()
        Me.txtTech = New System.Windows.Forms.TextBox()
        Me.lblType = New System.Windows.Forms.Label()
        Me.cboType = New System.Windows.Forms.ComboBox()
        Me.lblMaxTech = New System.Windows.Forms.Label()
        Me.numMaxTech = New System.Windows.Forms.NumericUpDown()
        Me.lblPlayers = New System.Windows.Forms.Label()
        Me.numPlayers = New System.Windows.Forms.NumericUpDown()
        Me.chkUnitOnly = New System.Windows.Forms.CheckBox()
        Me.lblAiPlayers = New System.Windows.Forms.Label()
        Me.numAiPlayers = New System.Windows.Forms.NumericUpDown()
        Me.lblDllInfo = New System.Windows.Forms.Label()
        Me.lblNote = New System.Windows.Forms.Label()
        Me.btnOK = New System.Windows.Forms.Button()
        Me.btnCancel = New System.Windows.Forms.Button()
        CType(Me.numMaxTech, System.ComponentModel.ISupportInitialize).BeginInit()
        CType(Me.numPlayers, System.ComponentModel.ISupportInitialize).BeginInit()
        CType(Me.numAiPlayers, System.ComponentModel.ISupportInitialize).BeginInit()
        Me.SuspendLayout()
        '
        'lblName
        '
        Me.lblName.Location = New System.Drawing.Point(12, 21)
        Me.lblName.Name = "lblName"
        Me.lblName.Size = New System.Drawing.Size(100, 20)
        Me.lblName.TabIndex = 0
        Me.lblName.Text = "Name:"
        Me.lblName.TextAlign = System.Drawing.ContentAlignment.MiddleRight
        '
        'txtName
        '
        Me.txtName.Location = New System.Drawing.Point(120, 18)
        Me.txtName.Name = "txtName"
        Me.txtName.Size = New System.Drawing.Size(320, 20)
        Me.txtName.TabIndex = 1
        '
        'lblMap
        '
        Me.lblMap.Location = New System.Drawing.Point(12, 51)
        Me.lblMap.Name = "lblMap"
        Me.lblMap.Size = New System.Drawing.Size(100, 20)
        Me.lblMap.TabIndex = 2
        Me.lblMap.Text = "Map:"
        Me.lblMap.TextAlign = System.Drawing.ContentAlignment.MiddleRight
        '
        'txtMap
        '
        Me.txtMap.Location = New System.Drawing.Point(120, 48)
        Me.txtMap.Name = "txtMap"
        Me.txtMap.ReadOnly = True
        Me.txtMap.Size = New System.Drawing.Size(225, 20)
        Me.txtMap.TabIndex = 3
        '
        'btnMap
        '
        Me.btnMap.Location = New System.Drawing.Point(351, 47)
        Me.btnMap.Name = "btnMap"
        Me.btnMap.Size = New System.Drawing.Size(89, 23)
        Me.btnMap.TabIndex = 4
        Me.btnMap.Text = "Change..."
        Me.btnMap.UseVisualStyleBackColor = True
        '
        'lblTech
        '
        Me.lblTech.Location = New System.Drawing.Point(12, 81)
        Me.lblTech.Name = "lblTech"
        Me.lblTech.Size = New System.Drawing.Size(100, 20)
        Me.lblTech.TabIndex = 5
        Me.lblTech.Text = "Tech tree file:"
        Me.lblTech.TextAlign = System.Drawing.ContentAlignment.MiddleRight
        '
        'txtTech
        '
        Me.txtTech.Location = New System.Drawing.Point(120, 78)
        Me.txtTech.Name = "txtTech"
        Me.txtTech.Size = New System.Drawing.Size(320, 20)
        Me.txtTech.TabIndex = 6
        '
        'lblType
        '
        Me.lblType.Location = New System.Drawing.Point(12, 111)
        Me.lblType.Name = "lblType"
        Me.lblType.Size = New System.Drawing.Size(100, 20)
        Me.lblType.TabIndex = 7
        Me.lblType.Text = "Type:"
        Me.lblType.TextAlign = System.Drawing.ContentAlignment.MiddleRight
        '
        'cboType
        '
        Me.cboType.FormattingEnabled = True
        Me.cboType.Items.AddRange(New Object() {"Colony", "Tutorial", "AutoDemo", "MultiLastOneStanding", "MultiLandRush", "MultiSpaceRace", "MultiResourceRace", "MultiMidas"})
        Me.cboType.Location = New System.Drawing.Point(120, 108)
        Me.cboType.Name = "cboType"
        Me.cboType.Size = New System.Drawing.Size(160, 21)
        Me.cboType.TabIndex = 8
        '
        'lblMaxTech
        '
        Me.lblMaxTech.Location = New System.Drawing.Point(12, 141)
        Me.lblMaxTech.Name = "lblMaxTech"
        Me.lblMaxTech.Size = New System.Drawing.Size(100, 20)
        Me.lblMaxTech.TabIndex = 9
        Me.lblMaxTech.Text = "Max tech level:"
        Me.lblMaxTech.TextAlign = System.Drawing.ContentAlignment.MiddleRight
        '
        'numMaxTech
        '
        Me.numMaxTech.Location = New System.Drawing.Point(120, 138)
        Me.numMaxTech.Maximum = New Decimal(New Integer() {12, 0, 0, 0})
        Me.numMaxTech.Name = "numMaxTech"
        Me.numMaxTech.Size = New System.Drawing.Size(80, 20)
        Me.numMaxTech.TabIndex = 10
        '
        'lblPlayers
        '
        Me.lblPlayers.Location = New System.Drawing.Point(12, 171)
        Me.lblPlayers.Name = "lblPlayers"
        Me.lblPlayers.Size = New System.Drawing.Size(100, 20)
        Me.lblPlayers.TabIndex = 11
        Me.lblPlayers.Text = "Players:"
        Me.lblPlayers.TextAlign = System.Drawing.ContentAlignment.MiddleRight
        '
        'numPlayers
        '
        Me.numPlayers.Location = New System.Drawing.Point(120, 168)
        Me.numPlayers.Maximum = New Decimal(New Integer() {6, 0, 0, 0})
        Me.numPlayers.Minimum = New Decimal(New Integer() {1, 0, 0, 0})
        Me.numPlayers.Name = "numPlayers"
        Me.numPlayers.Size = New System.Drawing.Size(80, 20)
        Me.numPlayers.TabIndex = 12
        Me.numPlayers.Value = New Decimal(New Integer() {1, 0, 0, 0})
        '
        'chkUnitOnly
        '
        Me.chkUnitOnly.Location = New System.Drawing.Point(120, 196)
        Me.chkUnitOnly.Name = "chkUnitOnly"
        Me.chkUnitOnly.Size = New System.Drawing.Size(320, 20)
        Me.chkUnitOnly.TabIndex = 13
        Me.chkUnitOnly.Text = "Unit-only mission (most reports off)"
        Me.chkUnitOnly.UseVisualStyleBackColor = True
        '
        'lblAiPlayers
        '
        Me.lblAiPlayers.Location = New System.Drawing.Point(12, 225)
        Me.lblAiPlayers.Name = "lblAiPlayers"
        Me.lblAiPlayers.Size = New System.Drawing.Size(100, 20)
        Me.lblAiPlayers.TabIndex = 14
        Me.lblAiPlayers.Text = "AI players:"
        Me.lblAiPlayers.TextAlign = System.Drawing.ContentAlignment.MiddleRight
        '
        'numAiPlayers
        '
        Me.numAiPlayers.Location = New System.Drawing.Point(120, 222)
        Me.numAiPlayers.Maximum = New Decimal(New Integer() {5, 0, 0, 0})
        Me.numAiPlayers.Name = "numAiPlayers"
        Me.numAiPlayers.Size = New System.Drawing.Size(80, 20)
        Me.numAiPlayers.TabIndex = 15
        '
        'lblDllInfo
        '
        Me.lblDllInfo.Location = New System.Drawing.Point(12, 254)
        Me.lblDllInfo.Name = "lblDllInfo"
        Me.lblDllInfo.Size = New System.Drawing.Size(428, 18)
        Me.lblDllInfo.TabIndex = 16
        Me.lblDllInfo.Text = "These go into the mission DLL, which Save Mission writes with the placement."
        '
        'lblNote
        '
        Me.lblNote.ForeColor = System.Drawing.Color.DarkOrange
        Me.lblNote.Location = New System.Drawing.Point(12, 274)
        Me.lblNote.Name = "lblNote"
        Me.lblNote.Size = New System.Drawing.Size(428, 44)
        Me.lblNote.TabIndex = 17
        '
        'btnOK
        '
        Me.btnOK.DialogResult = System.Windows.Forms.DialogResult.OK
        Me.btnOK.Location = New System.Drawing.Point(265, 326)
        Me.btnOK.Name = "btnOK"
        Me.btnOK.Size = New System.Drawing.Size(85, 28)
        Me.btnOK.TabIndex = 18
        Me.btnOK.Text = "OK"
        Me.btnOK.UseVisualStyleBackColor = True
        '
        'btnCancel
        '
        Me.btnCancel.DialogResult = System.Windows.Forms.DialogResult.Cancel
        Me.btnCancel.Location = New System.Drawing.Point(355, 326)
        Me.btnCancel.Name = "btnCancel"
        Me.btnCancel.Size = New System.Drawing.Size(85, 28)
        Me.btnCancel.TabIndex = 19
        Me.btnCancel.Text = "Cancel"
        Me.btnCancel.UseVisualStyleBackColor = True
        '
        'fMissionProps
        '
        Me.AcceptButton = Me.btnOK
        Me.AutoScaleDimensions = New System.Drawing.SizeF(6.0!, 13.0!)
        Me.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font
        Me.CancelButton = Me.btnCancel
        Me.ClientSize = New System.Drawing.Size(452, 370)
        Me.Controls.Add(Me.btnCancel)
        Me.Controls.Add(Me.btnOK)
        Me.Controls.Add(Me.lblNote)
        Me.Controls.Add(Me.lblDllInfo)
        Me.Controls.Add(Me.numAiPlayers)
        Me.Controls.Add(Me.lblAiPlayers)
        Me.Controls.Add(Me.chkUnitOnly)
        Me.Controls.Add(Me.numPlayers)
        Me.Controls.Add(Me.lblPlayers)
        Me.Controls.Add(Me.numMaxTech)
        Me.Controls.Add(Me.lblMaxTech)
        Me.Controls.Add(Me.cboType)
        Me.Controls.Add(Me.lblType)
        Me.Controls.Add(Me.txtTech)
        Me.Controls.Add(Me.lblTech)
        Me.Controls.Add(Me.btnMap)
        Me.Controls.Add(Me.txtMap)
        Me.Controls.Add(Me.lblMap)
        Me.Controls.Add(Me.txtName)
        Me.Controls.Add(Me.lblName)
        Me.FormBorderStyle = System.Windows.Forms.FormBorderStyle.FixedDialog
        Me.MaximizeBox = False
        Me.MinimizeBox = False
        Me.Name = "fMissionProps"
        Me.ShowInTaskbar = False
        Me.StartPosition = System.Windows.Forms.FormStartPosition.CenterParent
        Me.Text = "Mission Properties"
        CType(Me.numMaxTech, System.ComponentModel.ISupportInitialize).EndInit()
        CType(Me.numPlayers, System.ComponentModel.ISupportInitialize).EndInit()
        CType(Me.numAiPlayers, System.ComponentModel.ISupportInitialize).EndInit()
        Me.ResumeLayout(False)
        Me.PerformLayout()

    End Sub

    Friend WithEvents lblName As System.Windows.Forms.Label
    Friend WithEvents txtName As System.Windows.Forms.TextBox
    Friend WithEvents lblMap As System.Windows.Forms.Label
    Friend WithEvents txtMap As System.Windows.Forms.TextBox
    Friend WithEvents btnMap As System.Windows.Forms.Button
    Friend WithEvents lblTech As System.Windows.Forms.Label
    Friend WithEvents txtTech As System.Windows.Forms.TextBox
    Friend WithEvents lblType As System.Windows.Forms.Label
    Friend WithEvents cboType As System.Windows.Forms.ComboBox
    Friend WithEvents lblMaxTech As System.Windows.Forms.Label
    Friend WithEvents numMaxTech As System.Windows.Forms.NumericUpDown
    Friend WithEvents lblPlayers As System.Windows.Forms.Label
    Friend WithEvents numPlayers As System.Windows.Forms.NumericUpDown
    Friend WithEvents chkUnitOnly As System.Windows.Forms.CheckBox
    Friend WithEvents lblAiPlayers As System.Windows.Forms.Label
    Friend WithEvents numAiPlayers As System.Windows.Forms.NumericUpDown
    Friend WithEvents lblDllInfo As System.Windows.Forms.Label
    Friend WithEvents lblNote As System.Windows.Forms.Label
    Friend WithEvents btnOK As System.Windows.Forms.Button
    Friend WithEvents btnCancel As System.Windows.Forms.Button
End Class
