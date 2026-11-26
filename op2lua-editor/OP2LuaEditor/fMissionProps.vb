' fMissionProps.vb
'
' Dialog for editing an OP2Lua mission's properties: name, map, tech tree file, type, max tech level, players,
' unit-only and multiplayer AI players - everything the mission DLL describes to the game (MissionDll.vb). Type is the
' game's mission type: Colony, Tutorial, AutoDemo, a multiplayer type (Multi...), which lists the mission in the
' multiplayer lobby, or a campaign mission's number (1 to 12); op2lua-newmission's --type names (LastOneStanding) are
' taken too. Used both by Mission > Properties (edit an open mission) and Mission > New Mission (seed a fresh one).
' The layout lives in fMissionProps.Designer.vb so it opens in the VS designer; this file only holds the in/out
' values and the event handlers.
'
' The caller seeds Props (and Note) before ShowDialog and reads Props back on DialogResult.OK.

Public Class fMissionProps

    Public Props As New MissionProps()

    ' Where the mission's DLL differs from these properties (shown under the fields), or Nothing.
    Public Note As String = Nothing

    ' Folder the "Change..." map dialog opens in (typically <OP2>\OPU).
    Public MapBrowseDir As String = ""

    Private Sub fMissionProps_Load(sender As Object, e As EventArgs) Handles MyBase.Load
        txtName.Text = Props.Name
        txtMap.Text = Props.MapFile
        txtTech.Text = Props.Tech
        cboType.Text = Props.TypeName
        numMaxTech.Value = Math.Min(Math.Max(Props.MaxTech, 0), 12)
        numPlayers.Value = Math.Min(Math.Max(Props.Players, 1), 6)
        chkUnitOnly.Checked = Props.UnitOnly
        numAiPlayers.Value = Math.Min(Math.Max(Props.AiPlayers, 0), 5)
        lblNote.Text = If(Note, "")
        UpdateAiPlayers()
        Try
            Me.Icon = My.Resources.AppIcon
        Catch
        End Try
    End Sub

    ' AI players (DescBlockEx) are for multiplayer types.
    Private Sub UpdateAiPlayers()
        Dim multi As Boolean = PlacementText.IsMultiplayerType(cboType.Text)
        numAiPlayers.Enabled = multi
        lblAiPlayers.Enabled = multi
    End Sub

    Private Sub cboType_TextChanged(sender As Object, e As EventArgs) Handles cboType.TextChanged
        UpdateAiPlayers()
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
        Dim p As New MissionProps()
        p.Name = txtName.Text.Trim()
        p.MapFile = txtMap.Text.Trim()
        p.Tech = txtTech.Text.Trim()
        p.TypeName = cboType.Text.Trim()
        p.MaxTech = CInt(numMaxTech.Value)
        p.Players = CInt(numPlayers.Value)
        p.UnitOnly = chkUnitOnly.Checked
        p.AiPlayers = CInt(numAiPlayers.Value)
        ' Everything here goes into the mission DLL, so it must be something the DLL can hold.
        Dim why As String = MissionDll.Problem(p)
        If why IsNot Nothing Then
            MessageBox.Show(Me, why, Me.Text, MessageBoxButtons.OK, MessageBoxIcon.Warning)
            Me.DialogResult = DialogResult.None
            Return
        End If
        Props = p
    End Sub

End Class
