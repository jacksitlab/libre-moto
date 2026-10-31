package net.osmand.aidlapi.map;

import android.os.Bundle;
import android.os.Parcel;
import java.util.ArrayList;
import java.util.List;
import net.osmand.aidlapi.AidlParams;

public class ARoadPolyline extends AidlParams {

	private ArrayList<ALatLon> points = new ArrayList<>();
	private String name = "";
	private String roadType = "";

	public ARoadPolyline() {
	}

	protected ARoadPolyline(Parcel in) {
		readFromParcel(in);
	}

	public static final Creator<ARoadPolyline> CREATOR = new Creator<ARoadPolyline>() {
		@Override
		public ARoadPolyline createFromParcel(Parcel in) {
			return new ARoadPolyline(in);
		}

		@Override
		public ARoadPolyline[] newArray(int size) {
			return new ARoadPolyline[size];
		}
	};

	public List<ALatLon> getPoints() { return points; }
	public void setPoints(List<ALatLon> pts) {
		points = new ArrayList<>();
		if (pts != null) points.addAll(pts);
	}
	public String getName() { return name; }
	public void setName(String n) { name = n != null ? n : ""; }
	public String getRoadType() { return roadType; }
	public void setRoadType(String t) { roadType = t != null ? t : ""; }

	@Override
	protected void readFromBundle(Bundle bundle) {
		name = bundle.getString("name", "");
		roadType = bundle.getString("roadType", "");
		ArrayList<ALatLon> pts = bundle.getParcelableArrayList("points");
		if (pts != null) points = pts;
	}

	@Override
	public void writeToBundle(Bundle bundle) {
		bundle.putString("name", name);
		bundle.putString("roadType", roadType);
		bundle.putParcelableArrayList("points", points);
	}
}
