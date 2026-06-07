' fMissionProps.vb
'
' Dialog for editing an OP2Lua mission's top-level metadata (name, map, tech file, type, max tech
' level, players). Used both by Mission > Properties (edit an open mission) and Mission > New Mission
' (seed a fresh one). The layout lives in fMissionProps.Designer.vb so it opens in the VS designer;
' this file only holds the in/out values and the event handlers.
'
' The caller seeds the public fields before ShowDialog and reads them back on DialogResult.OK.

Public Class fMissionProps

    Public MissionName As String = ""
    Public MapFile As String = ""
    Public Tech As String = "MULTITEK.TXT"
    Public MissionTypeName As String = "Colony"
    Public MaxTech As Integer = 12
    Public PlayersCount As Integer = 2

    ' Folder the "Change..." map dialog opens in (typically <OP2>\OPU).
    Public MapBrowseDir As String = ""

    Private Sub fMissionProps_Load(sender As Object, e As EventArgs) Handles MyBase.Load
        txtName.Text = MissionName
        txtMap.Text = MapFile
        txtTech.Text = Tech
        cboType.Text = MissionTypeName
        numMaxTech.Value = Math.Min(Math.Max(MaxTech, 0), 12)
        numPlayers.Value = Math.Min(Math.Max(PlayersCount, 1), 6)
        Try
            Me.Icon = My.Resources.AppIcon
        Catch
        End Try
    End Sub

    Private Sub btnMap_Click(sender As Object, e As EventArgs) Handles btnMap.Click
        Using dlg As New OpenFileDialog()
            dlg.Title = "Select the map for this mission"
            dlg.Filter = "Outpost 2 Map Files (*.map)|*.map|All Files (*.*)|*.*"
            If Not String.IsNullOrEmpty(MapBrowseDir) AndAlso IO.Directory.Exists(MapBrowseDir) Then
                dlg.InitialDirectory = MapBrowseDir
            End If
            If dlg.ShowDialog(Me) = DialogResult.OK Then
                txtMap.Text = IO.Path.GetFileName(dlg.FileName)
            End If
        End Using
    End Sub

    Private Sub btnOK_Click(sender As Object, e As EventArgs) Handles btnOK.Click
        If String.IsNullOrWhiteSpace(txtName.Text) Then
            MessageBox.Show(Me, "Name cannot be empty.", "Mission Properties", MessageBoxButtons.OK, MessageBoxIcon.Warning)
            Me.DialogResult = DialogResult.None
            Return
        End If
        If String.IsNullOrWhiteSpace(txtMap.Text) Then
            MessageBox.Show(Me, "A map must be selected.", "Mission Properties", MessageBoxButtons.OK, MessageBoxIcon.Warning)
            Me.DialogResult = DialogResult.None
            Return
        End If
        MissionName = txtName.Text.Trim()
        MapFile = txtMap.Text.Trim()
        Tech = txtTech.Text.Trim()
        MissionTypeName = cboType.Text.Trim()
        MaxTech = CInt(numMaxTech.Value)
        PlayersCount = CInt(numPlayers.Value)
    End Sub

End Class
